#include "urlfetch.hh"
#include "stamp.hh"
#include "proc.hh"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <memory>

#include <unistd.h>

#include <reproc++/reproc.hpp>

namespace fs = std::filesystem;

// One read of the tool's stdout.  Small: this is a trickle of progress lines,
// not a media stream.
static constexpr size_t READ_BUF = 4096;

struct UrlFetcher::Child
	{
	reproc::process proc;
	};

// ---- free helpers -----------------------------------------------------

bool urlfetch_http_url(const std::string& url)
	{
	return url.rfind("http://", 0) == 0 || url.rfind("https://", 0) == 0;
	}

std::optional<int> urlfetch_progress(const std::string& line)
	{
	// The first percentage on the line.  Deliberately generic: the handler
	// table names the tool, so nothing here may assume yt-dlp's phrasing.
	static const std::regex re(R"((\d+(?:\.\d+)?)\s*%)");
	std::smatch m;
	if (!std::regex_search(line, m, re)) return std::nullopt;
	double v = 0;
	try { v = std::stod(m[1].str()); }
	catch (...) { return std::nullopt; }
	if (v < 0)   v = 0;
	if (v > 100) v = 100;
	return static_cast<int>(v);
	}

std::optional<std::pair<int, int>> urlfetch_playlist_item(const std::string& line)
	{
	// yt-dlp's wording, unlike urlfetch_progress: a tool that phrases it
	// differently simply never counts as a playlist, which is the old
	// all-or-nothing behaviour rather than anything worse.
	static const std::regex re(R"(Downloading item (\d+) of (\d+))");
	std::smatch m;
	if (!std::regex_search(line, m, re)) return std::nullopt;
	try {
		int k = std::stoi(m[1].str()), n = std::stoi(m[2].str());
		if (k < 1 || n < 1 || k > n) return std::nullopt;
		return std::make_pair(k, n);
		}
	catch (...) { return std::nullopt; }
	}

std::vector<std::string> urlfetch_expand(const std::vector<std::string>& tmpl,
                                          const std::string& url,
                                          const std::string& dir)
	{
	std::vector<std::string> out;
	out.reserve(tmpl.size());
	for (const auto& a : tmpl) {
		if      (a == "%URL%") out.push_back(url);
		else if (a == "%DIR%") out.push_back(dir);
		else                   out.push_back(a);
		}
	return out;
	}

// ---- the table --------------------------------------------------------

std::vector<UrlHandler> UrlFetcher::default_handlers(int playlist_max)
	{
	// Every flag below is load-bearing; none of it is taste.
	//
	// --audio-format / --merge-output-format pin the *extension*, and that is a
	// correctness requirement rather than a preference.  A bare -x keeps
	// whatever container the site served - for YouTube frequently .webm holding
	// Opus - and MediaStore's extension tables are what decide whether a file is
	// a song or a film (is_video_ext() in codecs.hh derives it from the
	// extension alone).  A .webm song is filed as a video, probed with ffprobe,
	// given a video ladder and drawn as a black rectangle behind a play
	// triangle.
	//
	// The -o templates deliberately produce files two directories deep.  That is
	// what makes reorganise_by_tags() leave them alone (it only collects files
	// shallower than that), what lets the scan step enumerate artist
	// directories exactly as it does for an archive, and what satisfies
	// deleteUpload's exact five-component path check.  A shallower result is
	// never deletable by its owner.
	//
	// -P takes the directory and -o takes the template; %DIR% must not be put
	// inside -o, where % is significant and an uploads path containing one would
	// corrupt the template.
	//
	// --newline turns the tool's \r progress into whole lines and --progress
	// asks for that progress at all, which it otherwise suppresses when its
	// stdout is not a terminal - as it is not here.  (Nothing asks for colour
	// to be off: a pipe already gets none.)  The "--" is what stops a URL
	// beginning with a dash being read as an option.
	//
	// What one pasted URL can write into the uploads root is bounded by
	// --max-filesize per file and by -I per playlist. --no-playlist does not
	// do the second job: it only decides what a watch URL that also names a
	// list means, and a bare playlist URL is fetched whole regardless. It
	// stays so that a link copied from inside a playlist fetches the one video
	// it shows, which is what the person pasting it was looking at.
	//
	// The thumbnail output costs nothing and gains a cover: cover.jpg is the
	// first name in MediaStore's COVER_FILENAMES, so find_cover() picks it up
	// with no new code at all.
	//
	// --write-info-json is there for the same kind of reason, and buys the
	// chapter list. A concert or a DJ set arrives as one track; the uploader
	// wrote the timestamps and the site parsed them, and this is the only way
	// to be told. The batch pipeline converts the file's `chapters` array into
	// the sidecar gaindrive indexes and then deletes it -- see
	// convert_tool_sidecars() in gaindrive.cc.
	//
	// Not --embed-chapters: that writes them into the container, and only
	// sidecars are indexed, so the album view would never see them. And not
	// --write-description with our own parser, tempting though it is, since
	// parse_chapters() reads exactly the "13:35 Title" form a description
	// carries -- a description also carries timestamps in links and in quoted
	// comments, where `chapters` is the site's own parse.
	std::vector<std::string> common = {
		"--newline", "--progress", "--no-playlist",
		"-I", ":" + std::to_string(playlist_max > 0 ? playlist_max : 1),
		"--write-thumbnail", "--convert-thumbnails", "jpg",
		"--write-info-json", "-P", "%DIR%" };

	// Reading "Eric Clapton - I Shot The Sheriff (Live at Budokan 2009)" as
	// three fields rather than as one channel name.
	//
	// The site supplies `artist` and `album` only for tracks it serves with
	// music metadata - the auto-generated "Topic" channels. For everything else
	// those fields are absent and the output template falls through to the
	// *channel*, which is why an ordinary music video used to land under the
	// name of whoever posted it. The title is the only description there is, so
	// the title is what gets parsed. Same predicament as a video filename, and
	// the same shape of answer as videoname.cc, but this belongs here rather
	// than in C++ because it is the *tool's* naming stage: parsed before the
	// output template expands, so the directories are right the first time and
	// nothing has to be renamed afterwards.
	//
	// The order below is the whole design and is not arbitrary:
	//
	//  1. Strip the junk parenthetical *first*. "(Official Video)", "[4K]" and
	//     "(Remastered)" are the commonest thing in a bracket on YouTube, and
	//     taking a bracket as the album without this files half a collection
	//     under an album called "Official Video". A bracket is junk when it
	//     *contains* a junk word, since "(Live at Pompeii) [HD]" is one of each.
	//  2. Then a bracket that is only a year or only a bare noun.
	//  3. Then the general "Artist - Track" split, and
	//  4. then the specific "Artist - Track (Album)" split, which overwrites
	//     what 3 wrote when it applies.
	//
	// 3 and 4 both read the *original* title and neither writes it back, so the
	// specific one does not depend on the general one having run - the two are
	// ordered only so the more specific result wins. A rule that matches
	// nothing is not an error; yt-dlp says so on stdout and carries on.
	//
	// The year is deliberately *kept* in the album. "Live at Budokan 2009" and
	// "Live at Budokan 1978" are two concerts, and dropping the year merges
	// them into one album.
	//
	// What this cannot do is tell "Work - Performer" from "Performer - Work":
	// "Beethoven Symphony No. 5 - Karajan" comes out with the conductor as the
	// track. Nothing in the string says which way round it is.
	const std::string junk_bracket =
		R"(\s*[\(\[][^)\]]*\b(?i:official|lyrics?|visuali[sz]er|remaster(ed)?)"
		R"(|explicit|hd|hq|4k|8k|m/?v|full\s*album|clip\s*officiel|topic)\b)"
		R"([^)\]]*[\)\]])";
	const std::string junk_only =
		R"(\s*[\(\[]\s*(?i:(19|20)\d{2}|audio|video|music\s*video)\s*[\)\]])";
	const std::string split_artist =
		R"(title:(?P<artist>.+?)\s+[-–—]\s+(?P<track>.+))";
	const std::string split_album =
		R"(title:(?P<artist>.+?)\s+[-–—]\s+(?P<track>.+?)\s*)"
		R"([\(\[](?P<album>[^()\[\]]+)[\)\]]\s*$)";

	std::vector<std::string> naming = {
		"--replace-in-metadata", "title", junk_bracket, "",
		"--replace-in-metadata", "title", junk_only,    "",
		"--parse-metadata",      split_artist,
		"--parse-metadata",      split_album };

	// Falling back through album_artist before the channel, and to the channel
	// only when the title said nothing at all - at which point it is usually
	// not music, and a lecture or podcast really is best filed under whoever
	// published it. An operator who would rather see them collected together
	// can end the chain with a literal instead: %(artist|Unknown Artist)s.
	//
	// A playlist's entries are filed under the playlist and its owner first,
	// whatever each title says, because the point of fetching one is a single
	// album in the playlist's order: per-entry names would scatter it across
	// as many artists as it has videos. Those fields are absent outside a
	// playlist, so a single fetch falls straight through to the old chain.
	// A typed artist or album still overrides both, in apply_batch_names().
	const std::string dir_artist =
		"%(playlist_uploader,artist,album_artist,uploader)s";
	// A track with no album of its own is an album of one, which is what
	// falling through to the track name means here.
	const std::string dir_album  = "%(playlist_title,album,track,title)s";
	// The playlist position as a leading "07 - ", which is the only track
	// number a video can carry: no container gaindrive reads has a tag for
	// it, and the scanner takes it from the name (parse_video_name()).
	// Padding is not relied on: the scanner orders by the parsed number, not
	// by the filename.
	const std::string file_stem  =
		"%(playlist_index&{} - |)s%(track,title)s";

	auto with = [&](std::vector<std::string> head) {
		std::vector<std::string> v = std::move(head);
		v.insert(v.end(), common.begin(), common.end());
		v.insert(v.end(), naming.begin(), naming.end());
		v.push_back("-o");
		v.push_back(dir_artist + "/" + dir_album + "/" + file_stem + ".%(ext)s");
		v.push_back("-o");
		v.push_back("thumbnail:" + dir_artist + "/" + dir_album
		            + "/cover.%(ext)s");
		v.push_back("--");
		v.push_back("%URL%");
		return v;
		};

	UrlHandler yt;
	yt.name    = "yt-dlp";
	// Anchored and whole-matched.  Everything yt-dlp supports is far more than
	// this, and widening it is the operator's call, made in the config file
	// where the consequences are written down.
	yt.pattern = R"(https?://(www\.|m\.|music\.)?(youtube\.com|youtu\.be)/.*)";
	// Audio carries the position as a tag as well, since a tag is what the
	// scanner prefers for audio and the filename prefix is only its fallback.
	yt.audio_argv = with({ "yt-dlp", "-x", "--audio-format", "opus",
	                       "--embed-metadata", "--max-filesize", "2G",
	                       "--parse-metadata",
	                       "playlist_index:%(track_number)s" });
	yt.video_argv = with({ "yt-dlp", "--merge-output-format", "mp4",
	                       "--embed-metadata", "--max-filesize", "8G" });
	return { yt };
	}

// What a playlist fetch that lost some entries leaves behind, reduced to the
// entries that finished. Returns how many did.
//
// Removed: the tool's part files (.part, .ytdl, fragments), the per-format
// streams of a merge that never happened (Title.f137.mp4) and its .temp.
// output, and any .info.json whose media is gone - written before the
// download starts, it would otherwise become a chapter sidecar for nothing.
// Counted: whatever is left that is neither metadata nor a cover.
static int prune_partial_items(const fs::path& dir)
	{
	static const std::regex partial(
		R"(.*(\.part|\.ytdl|\.part-Frag\d+)$|.*\.temp\..*|.*\.f\d+(-\d+)?\.[^.]+$)");
	static constexpr std::string_view INFO = ".info.json";
	auto ends_with = [](const std::string& s, std::string_view suf) {
		return s.size() > suf.size()
		    && s.compare(s.size() - suf.size(), suf.size(), suf) == 0;
		};

	std::error_code ec;
	std::vector<fs::path> files;
	for (auto& e : fs::recursive_directory_iterator(dir, ec))
		if (e.is_regular_file(ec)) files.push_back(e.path());

	std::vector<fs::path> kept;
	for (const auto& f : files) {
		if (std::regex_match(f.filename().string(), partial)) {
			std::error_code rec;
			fs::remove(f, rec);
			}
		else
			kept.push_back(f);
		}

	int items = 0;
	for (const auto& f : kept) {
		const std::string name = f.filename().string();
		if (ends_with(name, INFO)) {
			const std::string base = name.substr(0, name.size() - INFO.size()) + ".";
			bool has_media = std::any_of(kept.begin(), kept.end(),
				[&](const fs::path& o) {
					const std::string on = o.filename().string();
					return o.parent_path() == f.parent_path()
					    && on.rfind(base, 0) == 0 && !ends_with(on, INFO);
					});
			if (!has_media) {
				std::error_code rec;
				fs::remove(f, rec);
				}
			continue;
			}
		std::string ext = f.extension().string();
		std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
		if (f.stem() == "cover" || ext == ".jpg" || ext == ".jpeg"
		    || ext == ".png" || ext == ".webp" || ext == ".json")
			continue;
		items++;
		}
	return items;
	}

// ---- UrlFetcher -------------------------------------------------------

UrlFetcher::UrlFetcher(const std::optional<std::vector<UrlHandler>>& handlers,
                       int timeout_s, int playlist_max)
	: timeout_s_(timeout_s > 0 ? timeout_s : 2 * 60 * 60)
	{
	auto table = handlers ? *handlers : default_handlers(playlist_max);

	for (auto& h : table) {
		if (h.name.empty()) h.name = h.pattern;
		if (h.audio_argv.empty() && h.video_argv.empty()) {
			std::cout << stamp() << "url fetch: handler \"" << h.name
			          << "\" has no argv for either mode; ignored" << std::endl;
			continue;
			}
		// Compiled here rather than at match time.  A std::regex_error thrown
		// on an httplib thread is a bare 500 with nothing in the log, and the
		// operator's mistake deserves to be named at startup - the same reason
		// roots and cast devices are validated before listen().
		try {
			h.re = std::regex(h.pattern, std::regex::ECMAScript
			                             | std::regex::icase);
			}
		catch (const std::exception& e) {
			std::cout << stamp() << "url fetch: handler \"" << h.name
			          << "\" has an invalid pattern (" << e.what()
			          << "); ignored" << std::endl;
			continue;
			}
		// A missing tool is not an error either: it is a handler that cannot
		// run, and the client is better told there is none than told there is
		// one that fails.
		std::string prog = !h.audio_argv.empty() ? h.audio_argv.front()
		                                          : h.video_argv.front();
		if (!on_path(prog)) {
			std::cout << stamp() << "url fetch: handler \"" << h.name
			          << "\" needs " << prog
			          << ", which is not on PATH; ignored" << std::endl;
			continue;
			}
		handlers_.push_back(std::move(h));
		}

	if (handlers_.empty())
		std::cout << stamp() << "url fetch: no usable handlers; "
		          << "fetching from a URL is disabled" << std::endl;
	else {
		std::cout << stamp() << "url fetch: " << handlers_.size()
		          << " handler(s):";
		for (const auto& h : handlers_) std::cout << ' ' << h.name;
		std::cout << std::endl;
		}
	}

UrlFetcher::~UrlFetcher() = default;

std::vector<UrlFetcher::Capability> UrlFetcher::capabilities() const
	{
	std::vector<Capability> out;
	for (const auto& h : handlers_)
		out.push_back({ h.name, !h.audio_argv.empty(), !h.video_argv.empty() });
	return out;
	}

const UrlHandler* UrlFetcher::match(const std::string& url) const
	{
	// The scheme first, so no pattern however loose can admit file:// or
	// gopher://.  regex_match and not regex_search, so no pattern however loose
	// can be satisfied by a site name appearing in a fragment.
	if (!urlfetch_http_url(url)) return nullptr;
	for (const auto& h : handlers_)
		if (std::regex_match(url, h.re)) return &h;
	return nullptr;
	}

bool UrlFetcher::cancel(const std::string& job_id)
	{
	std::lock_guard<std::mutex> lk(mu_);
	if (!child_ || child_id_ != job_id) return false;
	// SIGKILL, not SIGTERM, for the reason serve_transcoded gives: a tool
	// asked politely may sit flushing to a pipe nobody is reading.  Killing
	// under the mutex is what makes the pointer safe - run() clears it under
	// the same mutex before its Child leaves scope.
	child_->proc.kill();
	return true;
	}

bool UrlFetcher::cancel_any()
	{
	std::lock_guard<std::mutex> lk(mu_);
	if (!child_) return false;
	child_->proc.kill();
	return true;
	}

UrlFetcher::Result UrlFetcher::run(
	const UrlHandler& h, bool audio, const std::string& url,
	const fs::path& dir, const std::string& job_id,
	const std::function<void(int, const std::string&)>& progress)
	{
	Result r;

	const auto& tmpl = audio ? h.audio_argv : h.video_argv;
	if (tmpl.empty()) {
		r.error = "This handler cannot fetch that.";
		return r;
		}
	auto argv = urlfetch_expand(tmpl, url, dir.string());

	{
	std::string line;
	for (const auto& a : argv) { line += ' '; line += a; }
	std::cout << stamp() << "url fetch: running:" << line << std::endl;
	}

	reproc::options opts;
	// stderr to a temp file, never a pipe: the read loop below drains stdout
	// only, so a chatty tool would block for ever once a stderr pipe filled.
	std::unique_ptr<FILE, int(*)(FILE*)> errf(std::tmpfile(), &std::fclose);
	if (errf) {
		opts.redirect.err.type = reproc::redirect::type::file_;
		opts.redirect.err.file = errf.get();
		}
	else
		opts.redirect.err.type = reproc::redirect::type::discard;
	opts.redirect.out.type = reproc::redirect::type::pipe;
	// No opts.deadline: the limit is per item and is enforced by the read
	// loop, since a whole-job deadline is either too short for a playlist or
	// far too long for one stalled video.

	Child child;
	if (auto ec = child.proc.start(argv, opts)) {
		std::cout << stamp() << "url fetch: launch failed: " << ec.message()
		          << std::endl;
		r.error = "Could not start " + argv.front() + ".";
		return r;
		}

	{
	std::lock_guard<std::mutex> lk(mu_);
	child_    = &child;
	child_id_ = job_id;
	}
	// Registered for exactly as long as the process exists.  Every path out of
	// this function goes through here before `child` leaves scope, so cancel()
	// can never reach a destroyed process.
	struct Unregister
		{
		UrlFetcher* self;
		~Unregister()
			{
			std::lock_guard<std::mutex> lk(self->mu_);
			self->child_ = nullptr;
			self->child_id_.clear();
			}
		} unreg{ this };

	using clock = std::chrono::steady_clock;
	const auto item_limit = std::chrono::seconds(timeout_s_);
	auto       item_start = clock::now();
	bool       timed_out  = false;
	// From the tool's "item k of n" lines; items stays 0 for a single video.
	int        item = 0, items = 0;

	std::string pending;
	int         percent = 0;
	uint8_t     buf[READ_BUF];
	std::error_code read_ec;
	for (;;) {
		// Polled with a timeout rather than read straight away, so the limit is
		// checked while the tool prints nothing - and a stalled download is
		// exactly when it prints nothing.
		auto [events, perr] = child.proc.poll(reproc::event::out,
		                                      reproc::milliseconds(1000));
		if (clock::now() - item_start > item_limit) {
			timed_out = true;
			// Under the mutex for the same reason cancel() takes it.
			std::lock_guard<std::mutex> lk(mu_);
			child.proc.kill();
			break;
			}
		if (perr == std::errc::timed_out || (!perr && events == 0)) continue;
		// Anything else - output, a closed pipe, an error - is read()'s to
		// report.
		auto [n, err] = child.proc.read(reproc::stream::out, buf, sizeof(buf));
		// Check err before n: reproc wraps a negative C return value into
		// size_t, so err is the reliable EOF/error indicator and n may be a
		// huge positive number when it is set.
		if (n == 0 || err) {
			if (err && err != std::make_error_code(std::errc::broken_pipe))
				read_ec = err;
			break;
			}
		pending.append(reinterpret_cast<const char*>(buf), n);
		// Split on either terminator.  --newline asks for '\n', but a handler
		// the operator wrote may name a tool that only ever emits '\r'.
		size_t pos;
		while ((pos = pending.find_first_of("\r\n")) != std::string::npos) {
			std::string ln = pending.substr(0, pos);
			pending.erase(0, pos + 1);
			if (ln.empty()) continue;
			if (auto it = urlfetch_playlist_item(ln)) {
				item       = it->first;
				items      = it->second;
				item_start = clock::now();
				percent    = 0;
				}
			// Keep the previous percentage when a line carries none, or the bar
			// snaps to zero for the whole post-processing phase.
			if (auto p = urlfetch_progress(ln)) percent = *p;
			// One bar for the whole playlist, not one that refills per entry.
			int overall = items > 1 ? ((item - 1) * 100 + percent) / items
			                        : percent;
			std::cout << stamp() << "url fetch: " << ln << std::endl;
			if (progress) progress(overall, ln);
			}
		// A very long line with no terminator must not grow without bound.
		if (pending.size() > 64 * 1024) pending.clear();
		}

	auto [status, wec] = child.proc.wait(reproc::infinite);

	std::error_code fec;
	for (auto& e : fs::recursive_directory_iterator(dir, fec))
		if (e.is_regular_file(fec)) r.files++;

	bool failed = timed_out || read_ec || wec || status != 0 || r.files == 0;

	// A playlist almost always holds an entry that is deleted, private or
	// blocked here, and the tool then exits non-zero after fetching the rest.
	// Throwing those away would mean no real playlist ever arrives, so the
	// finished entries are kept and the job says how many are missing. A
	// single video stays all-or-nothing: there is nothing partial worth
	// keeping.
	if (failed && items > 1) {
		int kept = prune_partial_items(dir);
		if (kept > 0) {
			r.files = 0;
			for (auto& e : fs::recursive_directory_iterator(dir, fec))
				if (e.is_regular_file(fec)) r.files++;
			r.ok          = true;
			r.items_ok    = kept;
			r.items_total = items;
			r.error = (timed_out ? "Timed out on item " + std::to_string(item)
			                       + "; " : std::string())
			        + std::to_string(kept) + " of " + std::to_string(items)
			        + " items fetched.";
			std::cout << stamp() << "url fetch: partial playlist, " << kept
			          << " of " << items << " item(s) into " << dir
			          << std::endl;
			if (errf) {
				std::string tail = stderr_tail(errf.get());
				if (!tail.empty())
					std::cout << stamp() << "url fetch: stderr: " << tail
					          << std::endl;
				}
			return r;
			}
		}

	if (failed) {
		std::cout << stamp() << "url fetch: failed status=" << status
		          << " files=" << r.files
		          << (read_ec ? " read=" + read_ec.message() : "")
		          << (wec     ? " wait=" + wec.message()     : "")
		          << std::endl;
		if (errf) {
			std::string tail = stderr_tail(errf.get());
			if (!tail.empty())
				std::cout << stamp() << "url fetch: stderr: " << tail
				          << std::endl;
			}
		// A clean exit that produced nothing is a failure, the same rule the
		// transcode cache applies to a zero-byte output: there is no library
		// entry to be made from it and reporting success would be a lie.
		r.error = timed_out ? std::string("The fetch timed out.")
		    : status != 0
		    ? argv.front() + " failed (exit " + std::to_string(status) + ")."
		    : (r.files == 0 ? "The fetch produced no files."
		                    : "The fetch could not be read.");
		r.files = 0;
		return r;
		}

	std::cout << stamp() << "url fetch: " << r.files << " file(s) into " << dir
	          << std::endl;
	r.ok = true;
	if (items > 1) r.items_ok = r.items_total = items;
	return r;
	}
