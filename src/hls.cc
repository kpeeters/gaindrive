#include "hls.hh"
#include "dvd.hh"
#include "fmp4.hh"
#include "proc.hh"
#include "stamp.hh"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <iostream>
#include <sstream>

#include <signal.h>

#include <reproc++/drain.hpp>
#include <reproc++/reproc.hpp>

namespace fs = std::filesystem;
using Clock  = std::chrono::steady_clock;

namespace {

// Aimed for, not exact: a copied film can only be cut on its keyframes.
constexpr double SEGMENT_SECS = 6.0;
// A final segment shorter than this joins the one before it.
constexpr double MIN_TAIL     = 2.0;

// Fragmented, one fragment per keyframe, and a moov written only once the
// first packets are known (delay_moov) so its edit lists say where each track
// starts - fmp4.hh reads them from there.  negative_cts_offsets gives the
// first video sample a composition offset of zero, so a B-frame stream's first
// picture sits exactly on its decode time instead of a frame or two after it.
constexpr const char* MOVFLAGS =
	"frag_keyframe+empty_moov+delay_moov+default_base_moof+negative_cts_offsets";
// The init segment comes from a different run than the media segments, so
// everything its moov declares has to come out the same in every run.  The
// video timescale does not on its own: ffmpeg's segment muxer hands the mp4
// muxer a different stream time base than a plain mp4 output gets, and the
// two runs pick 12288 and 16000 for the same film.  90 kHz holds Matroska's
// milliseconds and every common frame rate exactly.
constexpr const char* VIDEO_TIMESCALE = "90000";

// One stateless segment or init run.
constexpr auto RUN_DEADLINE = reproc::milliseconds(60000);

// Session tuning.  A request this far past what the running encoder has
// written waits for it rather than restarting it, which is what playing
// straight on looks like; further than that is a seek.
constexpr size_t   WAIT_AHEAD   = 3;
constexpr auto     WAIT_LIMIT   = std::chrono::seconds(20);
// The encoder is paused once it is this many segments past the last request,
// so an idle viewer does not have the rest of the film encoded for nothing.
constexpr size_t   RUN_AHEAD    = 10;
constexpr size_t   KEEP_BEHIND  = 10;
constexpr auto     IDLE_LIMIT   = std::chrono::seconds(120);
constexpr size_t   MAX_SESSIONS = 8;

std::string secs(double t)
	{
	char buf[32];
	std::snprintf(buf, sizeof(buf), "%.6f", t);
	return buf;
	}

void append(std::vector<std::string>& a, std::initializer_list<std::string> more)
	{
	a.insert(a.end(), more.begin(), more.end());
	}

// Everything up to and including the stream maps.  `copyts` keeps the source's
// own timestamps, which is what a copied cut has to be expressed in: a cut
// "before E" must mean the keyframe that is at E in the file.  A re-encode
// instead runs on the plain timeline that starts at `start`, which ffmpeg's
// accurate seek makes exact.
std::vector<std::string> front(const Streamer::SongInfo& song, double start,
                               bool copyts)
	{
	std::vector<std::string> a = { "ffmpeg", "-nostdin" };
	// No seek at all from the very start.  Even -ss 0 is a seek, and after one
	// ffmpeg (6.1 as much as 4.4) can start an AVI's audio well after its
	// video - 1.6 s of silence on an XviD/AC3 test file.
	if (start > 0) append(a, { "-ss", secs(start) });
	if (copyts) a.push_back("-copyts");
	append(a, { "-i", dvd_input(song.path),
	            "-map", "0:v:0", "-map", "0:a:0?", "-map_metadata", "-1" });
	return a;
	}

// The codec half of a run that encodes audio: copied video with AAC, or a
// full re-encode.
void session_codecs(std::vector<std::string>& a, const HlsPlan& p,
                    const HlsVariant& v)
	{
	if (p.copy_video) {
		append(a, { "-c:v", "copy", "-c:a", "aac", "-b:a", "128k", "-ac", "2",
		            "-copypriorss:v", "0" });
		return;
		}
	auto enc = Streamer::video_encode_args(v.size, v.kbps);
	a.insert(a.end(), enc.begin(), enc.end());
	}

// Segment k of a fully copied plan, or with `init` the short run whose moov
// becomes the init segment.  The run goes a second past E, and fmp4_media()
// cuts every stream at E exactly: -to stops on decode time, which would keep
// the next segment's keyframe and its first B-frames.
std::vector<std::string> copy_argv(const Streamer::SongInfo& song,
                                   const HlsPlan& p, size_t k, bool init)
	{
	const double S    = p.bounds[k];
	const double E    = p.bounds[k + 1];
	const bool   last = k + 1 == p.segments();
	auto a = front(song, S, true);
	append(a, { "-c", "copy", "-copypriorss", "0",
	            "-avoid_negative_ts", "disabled" });
	if (init)
		append(a, { "-to", secs(S + 1), "-use_editlist", "0" });
	else if (!last)
		append(a, { "-to", secs(E + 1) });
	append(a, { "-video_track_timescale", VIDEO_TIMESCALE,
	            "-f", "mp4", "-movflags", MOVFLAGS, "pipe:1" });
	return a;
	}

// The short run that provides a session plan's init segment.  The codec
// configuration does not depend on where a run starts, so any run's moov
// describes every segment; this one starts at the beginning because that is
// always there.
std::vector<std::string> session_init_argv(const Streamer::SongInfo& song,
                                           const HlsPlan& p, const HlsVariant& v)
	{
	const double S = p.bounds[0];
	auto a = front(song, S, p.copy_video);
	session_codecs(a, p, v);
	append(a, { "-avoid_negative_ts", "disabled",
	            "-to", secs(p.copy_video ? S + 1 : 1), "-use_editlist", "0",
	            "-video_track_timescale", VIDEO_TIMESCALE,
	            "-f", "mp4", "-movflags", MOVFLAGS, "pipe:1" });
	return a;
	}

// One continuous run from segment n to the end, cut by ffmpeg's segment muxer
// at the plan's boundaries.  Its split times are relative to the run's start;
// a re-encode also forces a keyframe at each, which is what the muxer cuts on.
std::vector<std::string> session_argv(const Streamer::SongInfo& song,
                                      const HlsPlan& p, const HlsVariant& v,
                                      size_t n, const fs::path& dir)
	{
	const double S = p.bounds[n];
	auto a = front(song, S, p.copy_video);
	session_codecs(a, p, v);

	std::string cuts, keys;
	for (size_t k = n + 1; k < p.segments(); ++k) {
		// Half a tick early, so a keyframe exactly on the boundary is never
		// lost to rounding in the comparison.
		cuts += (cuts.empty() ? "" : ",") + secs(p.bounds[k] - S - p.tick / 2);
		keys += (keys.empty() ? "" : ",") + secs(p.bounds[k] - S);
		}
	if (!p.copy_video && !keys.empty()) append(a, { "-force_key_frames", keys });
	append(a, { "-avoid_negative_ts", "disabled", "-f", "segment" });
	if (!cuts.empty()) append(a, { "-segment_times", cuts });
	append(a, { "-segment_start_number", std::to_string(n),
	            "-segment_format", "mp4",
	            "-segment_format_options", std::string("movflags=") + MOVFLAGS
	                                     + ":video_track_timescale=" + VIDEO_TIMESCALE,
	            "-segment_list", (dir / "list").string(),
	            "-segment_list_type", "flat",
	            (dir / "%d.mp4").string() });
	return a;
	}

std::string log_argv(const std::vector<std::string>& a)
	{
	std::string cmd;
	for (const auto& s : a) { cmd += ' '; cmd += s; }
	return cmd;
	}

// Runs one short ffmpeg to completion and returns its stdout.
std::optional<std::string> run_capture(const std::vector<std::string>& argv)
	{
	reproc::process proc;
	reproc::options opts;
	// A file rather than a pipe: nothing drains stderr while stdout is read.
	auto errf = std::shared_ptr<FILE>(std::tmpfile(),
		[](FILE* f){ if (f) std::fclose(f); });
	if (errf) {
		opts.redirect.err.type = reproc::redirect::type::file_;
		opts.redirect.err.file = errf.get();
		}
	else
		opts.redirect.err.type = reproc::redirect::type::discard;
	opts.deadline = RUN_DEADLINE;

	if (auto ec = proc.start(argv, opts)) {
		std::cout << stamp() << "hls: ffmpeg launch failed: " << ec.message()
		          << std::endl;
		return std::nullopt;
		}
	std::string          out;
	reproc::sink::string sink(out);
	auto ec = reproc::drain(proc, sink, reproc::sink::null);
	if (ec) proc.kill();
	auto [status, wec] = proc.wait(reproc::infinite);
	if (ec || wec || status != 0 || out.empty()) {
		std::cout << stamp() << "hls: ffmpeg failed (status " << status
		          << (ec ? ", " + ec.message() : std::string()) << "):"
		          << log_argv(argv) << std::endl;
		auto tail = errf ? stderr_tail(errf.get()) : std::string();
		if (!tail.empty())
			std::cout << stamp() << "hls: ffmpeg stderr: " << tail << std::endl;
		return std::nullopt;
		}
	return out;
	}

std::optional<std::string> read_file(const fs::path& p)
	{
	std::ifstream in(p, std::ios::binary);
	if (!in) return std::nullopt;
	std::ostringstream ss;
	ss << in.rdbuf();
	return ss.str();
	}

void send_mp4(httplib::Response& res, std::string bytes)
	{
	res.set_content(std::move(bytes), "video/mp4");
	}

}

// ---- one viewer's running encoder -------------------------------------

struct Hls::Session {
	std::string                      owner;
	std::string                      film;
	Streamer::SongInfo               song;
	HlsVariant                       variant;
	HlsPlan                          plan;
	fs::path                         dir;

	// Everything below is guarded by mu.  It is held while the encoder is
	// started, polled or read, and never while a request waits for it.
	std::mutex                       mu;
	std::unique_ptr<reproc::process> proc;
	std::shared_ptr<FILE>            errf;
	int                              pid     = 0;
	bool                             running = false;
	bool                             paused  = false;
	// Segments [first, produced) of the current run are on disk.
	size_t                           first    = 0;
	size_t                           produced = 0;
	size_t                           pruned   = 0;
	size_t                           last_request = 0;
	// Seconds to add to the run's own timeline; known once its first segment
	// is written.
	bool                             have_shift = false;
	double                           shift      = 0.0;
	Clock::time_point                touched    = Clock::now();

	void stop()
		{
		if (!proc) return;
		proc->kill();
		proc->wait(reproc::infinite);
		proc.reset();
		running = false;
		paused  = false;
		}

	void clear_dir()
		{
		std::error_code ec;
		for (const auto& e : fs::directory_iterator(dir, ec))
			fs::remove(e.path(), ec);
		}

	void start(size_t n)
		{
		stop();
		clear_dir();
		first = produced = pruned = last_request = n;
		have_shift = false;

		auto argv = session_argv(song, plan, variant, n, dir);
		std::cout << stamp() << "hls: session " << dir.filename().string()
		          << " starts at segment " << n << ":" << log_argv(argv)
		          << std::endl;
		proc = std::make_unique<reproc::process>();
		reproc::options opts;
		errf = std::shared_ptr<FILE>(std::tmpfile(),
			[](FILE* f){ if (f) std::fclose(f); });
		if (errf) {
			opts.redirect.err.type = reproc::redirect::type::file_;
			opts.redirect.err.file = errf.get();
			}
		else
			opts.redirect.err.type = reproc::redirect::type::discard;
		opts.redirect.out.type = reproc::redirect::type::discard;
		if (auto ec = proc->start(argv, opts)) {
			std::cout << stamp() << "hls: ffmpeg launch failed: " << ec.message()
			          << std::endl;
			proc.reset();
			return;
			}
		running = true;
		auto [p, pec] = proc->pid();
		pid = pec ? 0 : p;
		}

	void signal_run(int sig)
		{
		if (running && pid > 0) ::kill(pid, sig);
		}

	// Picks up what the encoder has finished since the last look.  The segment
	// list names a file only once it is closed, so a listed file is complete.
	void poll()
		{
		if (running) {
			auto [status, ec] = proc->wait(reproc::milliseconds(0));
			if (ec != std::errc::timed_out) {
				running = false;
				paused  = false;
				if (ec || status != 0) {
					std::cout << stamp() << "hls: session "
					          << dir.filename().string()
					          << " encoder exited with status " << status
					          << std::endl;
					auto tail = errf ? stderr_tail(errf.get()) : std::string();
					if (!tail.empty())
						std::cout << stamp() << "hls: ffmpeg stderr: " << tail
						          << std::endl;
					}
				}
			}
		std::ifstream list(dir / "list");
		std::string   line;
		while (std::getline(list, line)) {
			auto dot = line.find('.');
			if (dot == 0 || dot == std::string::npos) continue;
			size_t k = 0;
			try { k = std::stoul(line.substr(0, dot)); }
			catch (...) { continue; }
			if (k >= first && k + 1 > produced) produced = k + 1;
			}
		if (!have_shift && produced > first) {
			// A re-encode's timeline starts at zero exactly where it was
			// asked to, so its first boundary is the whole offset.  A copy
			// keeps the file's timestamps, which ffmpeg still rebases a
			// little; its first picture is the keyframe the plan cut at.
			if (!plan.copy_video) {
				shift      = plan.bounds[first];
				have_shift = true;
				}
			else if (auto b = read_file(dir / (std::to_string(first) + ".mp4"))) {
				if (auto run = fmp4_parse(*b)) {
					shift      = plan.bounds[first] - fmp4_video_start(*run);
					have_shift = true;
					}
				}
			}
		}

	// Empty with `pending` set when the file is not finished yet: ffmpeg's
	// segment list can name a file before its last bytes are on disk, and a
	// truncated segment wedges the player's parser rather than merely losing
	// a few frames.
	std::optional<std::string> segment(size_t k, bool& pending)
		{
		pending = false;
		if (!have_shift) { pending = true; return std::nullopt; }
		auto b = read_file(dir / (std::to_string(k) + ".mp4"));
		if (!b) return std::nullopt;
		if (!fmp4_complete(*b)) { pending = true; return std::nullopt; }
		auto run = fmp4_parse(*b);
		if (!run) return std::nullopt;
		// Uncut: the segment muxer has already cut the run at the boundaries.
		return fmp4_media(*b, *run, shift, INFINITY);
		}

	// Called from the reaper: keep the encoder a bounded distance ahead and
	// the directory a bounded size.
	void tidy()
		{
		poll();
		if (running && !paused && produced > last_request + RUN_AHEAD) {
			signal_run(SIGSTOP);
			paused = true;
			}
		std::error_code ec;
		while (pruned + KEEP_BEHIND < last_request && pruned < produced) {
			fs::remove(dir / (std::to_string(pruned) + ".mp4"), ec);
			++pruned;
			}
		}
	};

// ---- Hls ----------------------------------------------------------------

Hls::Hls(fs::path work)
	: work_(std::move(work))
	{
	std::error_code ec;
	fs::remove_all(work_, ec);
	fs::create_directories(work_, ec);
	if (ec)
		std::cout << stamp() << "hls: cannot create " << work_.string() << ": "
		          << ec.message() << std::endl;
	reaper_ = std::thread([this]{
		// Guarded like every other long-lived thread here: an exception out of
		// a thread's top-level function is std::terminate.
		try { reap(); }
		catch (const std::exception& e) {
			std::cout << stamp() << "hls: reaper threw: " << e.what() << std::endl;
			}
		catch (...) {
			std::cout << stamp() << "hls: reaper threw" << std::endl;
			}
		});
	}

Hls::~Hls()
	{
	{
	std::lock_guard<std::mutex> lk(mu_);
	stopping_ = true;
	}
	reaper_cv_.notify_all();
	if (reaper_.joinable()) reaper_.join();
	for (auto& [owner, s] : sessions_) {
		std::lock_guard<std::mutex> lk(s->mu);
		s->stop();
		}
	std::error_code ec;
	fs::remove_all(work_, ec);
	}

bool HlsVariant::takes(const std::string& codec) const
	{
	std::string_view rest(audio);
	while (!rest.empty()) {
		auto comma = rest.find(',');
		if (rest.substr(0, comma) == codec) return true;
		if (comma == std::string_view::npos) break;
		rest.remove_prefix(comma + 1);
		}
	return false;
	}

std::string hls_audio_codecs(const std::string& param)
	{
	std::string out;
	for (const char* c : { "flac", "opus" }) {
		HlsVariant probe{ 0, "", param };
		if (!probe.takes(c)) continue;
		if (!out.empty()) out += ',';
		out += c;
		}
	return out;
	}

HlsPlan Hls::plan(const Streamer::SongInfo& song, const HlsVariant& v)
	{
	HlsPlan p;
	const double total = song.duration;

	// Copying needs a video every client decodes, no constraint on the
	// picture, and a place to cut: Matroska is the one container whose index
	// is cheap to read here, and it is also the one that most needs this, as
	// the files an MP4 player cannot open.
	if (!v.constrained() && song.codec == "mkv" && song.video_codec == "h264") {
		MkvKeyframes kf;
		{
		std::lock_guard<std::mutex> lk(cues_mu_);
		auto key = std::make_pair(song.id, song.file_modified);
		auto it  = cues_.find(key);
		if (it != cues_.end()) kf = it->second;
		else {
			kf = mkv_video_keyframes(song.path);
			if (cues_.size() >= 64) cues_.clear();
			cues_[key] = kf;
			}
		}
		if (kf.times.size() >= 2 && kf.times.front() < total) {
			p.copy_video = true;
			p.tick       = kf.tick;
			p.bounds.push_back(kf.times.front());
			for (double t : kf.times)
				if (t - p.bounds.back() >= SEGMENT_SECS && total - t >= MIN_TAIL)
					p.bounds.push_back(t);
			}
		}
	if (!p.copy_video) {
		for (double t = 0; t < total; t += SEGMENT_SECS) p.bounds.push_back(t);
		if (p.bounds.size() > 1 && total - p.bounds.back() < MIN_TAIL)
			p.bounds.pop_back();
		}
	p.bounds.push_back(total);

	// AAC is the one audio codec every HLS player takes in fragmented MP4;
	// anything else only for a client that said it plays it.
	p.copy_audio = p.copy_video
	            && (song.audio_codec.empty() || song.audio_codec == "aac"
	                || v.takes(song.audio_codec));
	return p;
	}

void Hls::serve_init(httplib::Response& res, const Streamer::SongInfo& song,
                     const HlsVariant& v)
	{
	HlsPlan p = plan(song, v);
	if (p.segments() == 0) { res.status = 404; return; }
	auto argv = p.stateless() ? copy_argv(song, p, 0, true)
	                          : session_init_argv(song, p, v);
	auto out  = run_capture(argv);
	auto run  = out ? fmp4_parse(*out) : std::nullopt;
	if (!run) { res.status = 500; return; }
	send_mp4(res, out->substr(0, run->first_moof));
	}

void Hls::serve_segment(httplib::Response& res, const Streamer::SongInfo& song,
                        const HlsVariant& v, size_t k, const std::string& owner)
	{
	HlsPlan p = plan(song, v);
	if (k >= p.segments()) { res.status = 404; return; }

	if (p.stateless()) {
		auto out = run_capture(copy_argv(song, p, k, false));
		auto run = out ? fmp4_parse(*out) : std::nullopt;
		// Half a tick early, so a sample exactly on the boundary goes to the
		// next segment whatever the rounding.
		const double end = k + 1 == p.segments()
		                 ? INFINITY : p.bounds[k + 1] - p.tick / 2;
		auto seg = run ? fmp4_media(*out, *run,
		                            p.bounds[k] - fmp4_video_start(*run), end)
		               : std::nullopt;
		if (!seg) { res.status = 500; return; }
		send_mp4(res, std::move(*seg));
		return;
		}

	auto s = session_for(song, v, p, owner);
	if (!s) { res.status = 503; return; }

	const auto deadline  = Clock::now() + WAIT_LIMIT;
	bool       restarted = false;
	for (;;) {
		{
		std::lock_guard<std::mutex> lk(s->mu);
		s->touched = Clock::now();
		s->poll();
		if (s->paused) {
			s->signal_run(SIGCONT);
			s->paused = false;
			}
		// Segments behind `pruned` were deleted to bound the directory; a
		// request for one is a seek back, like any other before `first`.
		const size_t oldest = std::max(s->first, s->pruned);
		bool pending = false;
		if (k >= oldest && k < s->produced) {
			s->last_request = k;
			auto seg = s->segment(k, pending);
			if (seg) {
				send_mp4(res, std::move(*seg));
				return;
				}
			// Unfinished is worth waiting for only while something can
			// still finish it.
			if (!pending || !s->running) {
				std::cout << stamp() << "hls: session "
				          << s->dir.filename().string()
				          << " cannot read segment " << k << std::endl;
				res.status = 500;
				return;
				}
			}
		bool coming = s->running && k >= oldest
		           && k < s->produced + WAIT_AHEAD;
		if (!pending && !coming) {
			if (restarted) {
				std::cout << stamp() << "hls: session "
				          << s->dir.filename().string()
				          << " ended without segment " << k << std::endl;
				res.status = 500;
				return;
				}
			s->start(k);
			restarted = true;
			if (!s->running) { res.status = 500; return; }
			}
		}
		if (Clock::now() > deadline) {
			std::cout << stamp() << "hls: session "
			          << s->dir.filename().string()
			          << " timed out waiting for segment " << k << std::endl;
			res.status = 503;
			return;
			}
		std::this_thread::sleep_for(std::chrono::milliseconds(100));
		}
	}

std::shared_ptr<Hls::Session> Hls::session_for(const Streamer::SongInfo& song,
                                               const HlsVariant& v,
                                               const HlsPlan& p,
                                               const std::string& owner)
	{
	const std::string film = std::to_string(song.id) + "/"
	                       + std::to_string(song.file_modified) + "/"
	                       + std::to_string(v.kbps) + "/" + v.size + "/"
	                       + v.audio;
	std::shared_ptr<Session> old, s;
	{
	std::lock_guard<std::mutex> lk(mu_);
	auto it = sessions_.find(owner);
	if (it != sessions_.end() && it->second->film == film) return it->second;
	// A different film for the same viewer: the old one has been left.
	if (it != sessions_.end()) {
		old = it->second;
		sessions_.erase(it);
		}
	if (sessions_.size() < MAX_SESSIONS) {
		s          = std::make_shared<Session>();
		s->owner   = owner;
		s->film    = film;
		s->song    = song;
		s->variant = v;
		s->plan    = p;
		s->dir     = work_ / std::to_string(++next_id_);
		std::error_code ec;
		fs::create_directories(s->dir, ec);
		sessions_[owner] = s;
		}
	else
		std::cout << stamp() << "hls: refusing a session, " << MAX_SESSIONS
		          << " already running" << std::endl;
	}
	if (old) {
		std::lock_guard<std::mutex> lk(old->mu);
		old->stop();
		std::error_code ec;
		fs::remove_all(old->dir, ec);
		}
	return s;
	}

void Hls::reap()
	{
	std::unique_lock<std::mutex> lk(mu_);
	while (!stopping_) {
		reaper_cv_.wait_for(lk, std::chrono::seconds(2));
		if (stopping_) break;
		std::vector<std::shared_ptr<Session>> idle, live;
		const auto now = Clock::now();
		for (auto it = sessions_.begin(); it != sessions_.end(); ) {
			bool gone;
			{
			std::lock_guard<std::mutex> slk(it->second->mu);
			gone = now - it->second->touched > IDLE_LIMIT;
			}
			if (gone) {
				idle.push_back(it->second);
				it = sessions_.erase(it);
				}
			else {
				live.push_back(it->second);
				++it;
				}
			}
		lk.unlock();
		for (auto& s : idle) {
			std::lock_guard<std::mutex> slk(s->mu);
			std::cout << stamp() << "hls: session "
			          << s->dir.filename().string() << " idle, ending"
			          << std::endl;
			s->stop();
			std::error_code ec;
			fs::remove_all(s->dir, ec);
			}
		for (auto& s : live) {
			std::lock_guard<std::mutex> slk(s->mu);
			s->tidy();
			}
		lk.lock();
		}
	}
