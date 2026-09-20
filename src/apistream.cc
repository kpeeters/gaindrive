#include "gaindrive.hh"
#include "subsonic.hh"
#include "authz.hh"
#include "stamp.hh"
#include "apientry.hh"
#include "textutil.hh"
#include "netaddr.hh"
#include "streamer.hh"
#include "codecs.hh"
#include "chapters.hh"
#include "untrusted.hh"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <vector>

#include <iostream>

#include <tinyxml2.h>
#include <nlohmann/json.hpp>

using namespace tinyxml2;

// Drops anything that is not well-formed UTF-8, and truncates only on a
// character boundary.
//
// The longest a single chapter title may be. A bound rather than a guess: the
// list is arbitrary text a client posted, and it comes straight back out
// through a JSON document and an XML attribute.
static constexpr size_t MAX_CHAPTER_NAME_BYTES = 500;

// The one place a chapter list becomes a response. Shared by getChapters,
// saveChapters and getAlbumChapters, so none of the three can describe the
// same markers differently — the panel is redrawn from the save's reply, and
// the album view from a fourth query, so a disagreement would show as the list
// changing under the user for no reason.
//
// `start` carries milliseconds because a save is a round trip: a client reads
// this list, edits one marker and writes the rest back, so truncating to whole
// seconds here would quietly flatten every fractional timestamp in a
// hand-written file. `duration` is whole seconds, matching Subsonic's Child,
// which is what a chapter row would become if these are ever listed as tracks.
// It is derived rather than stored — the next marker's start, or the video's
// own duration for the last, which only the server knows.
static nlohmann::json chapter_array_json(const std::vector<Chapter>& ch,
                                          double song_duration)
	{
	nlohmann::json list = nlohmann::json::array();
	for (size_t i = 0; i < ch.size(); i++) {
		double next = (i + 1 < ch.size()) ? ch[i + 1].start : song_duration;
		double d    = next - ch[i].start;
		list.push_back({
			{"index",    static_cast<int>(i + 1)},
			{"start",    std::llround(ch[i].start * 1000.0) / 1000.0},
			{"duration", d > 0 ? static_cast<int>(std::llround(d)) : 0},
			{"name",     ch[i].name} });
		}
	return list;
	}

static void chapter_array_xml(XMLDocument& doc, XMLElement* parent,
                               const std::vector<Chapter>& ch,
                               double song_duration)
	{
	for (size_t i = 0; i < ch.size(); i++) {
		double next = (i + 1 < ch.size()) ? ch[i + 1].start : song_duration;
		double d    = next - ch[i].start;
		auto* el = doc.NewElement("chapter");
		el->SetAttribute("index", static_cast<int>(i + 1));
		// Preformatted rather than handed to tinyxml2 as a double: its %.17g
		// would render 13.2 as 13.199999999999999.
		char buf[32];
		std::snprintf(buf, sizeof buf, "%.3f",
		              std::llround(ch[i].start * 1000.0) / 1000.0);
		el->SetAttribute("start",    buf);
		el->SetAttribute("duration", d > 0 ? static_cast<int>(std::llround(d)) : 0);
		el->SetAttribute("name",     ch[i].name.c_str());
		parent->InsertEndChild(el);
		}
	}

static void write_chapters_response(httplib::Response& res, int song_id,
                                     const MediaStore::VideoChapters& vc,
                                     double song_duration, bool writable,
                                     bool use_json)
	{
	std::string body;
	if (use_json)
		body = subsonic_ok_json([&](nlohmann::json& r) {
			r["chapters"] = {
				{"id",       sid(song_id)},
				{"source",   vc.source},
				{"writable", writable},
				{"chapter",  chapter_array_json(vc.chapters, song_duration)}
				};
			});
	else
		body = subsonic_ok([&](XMLDocument& doc, XMLElement* root) {
			auto* parent = doc.NewElement("chapters");
			parent->SetAttribute("id",       song_id);
			parent->SetAttribute("source",   vc.source.c_str());
			parent->SetAttribute("writable", writable);
			chapter_array_xml(doc, parent, vc.chapters, song_duration);
			root->InsertEndChild(parent);
			});
	res.set_content(body, use_json ? "application/json" : "application/xml");
	}


// Bounds for the web position table.  POS_STALE is when a report stops
// steering the pacer; POS_FORGET is when the row is dropped altogether.
static constexpr size_t POS_MAX_ENTRIES = 512;
static constexpr auto   POS_STALE  = std::chrono::seconds(30);
static constexpr auto   POS_FORGET = std::chrono::minutes(10);

void GainDrive::web_position_report(const std::string& user,
                                    const std::string& token,
                                    float pos, bool playing)
	{
	// A negative or NaN position would read as one of the streamer's control
	// sentinels and could abort the stream; clamp rather than trust.
	if (!(pos >= 0.0f)) pos = 0.0f;
	const auto now = std::chrono::steady_clock::now();
	const std::string key = user + "\n" + token;
	std::lock_guard<std::mutex> lock(web_pos_mu_);
	if (web_positions_.size() >= POS_MAX_ENTRIES
	    && !web_positions_.count(key)) {
		std::erase_if(web_positions_, [&](const auto& e) {
			return now - e.second.at > POS_FORGET;
			});
		if (web_positions_.size() >= POS_MAX_ENTRIES) {
			// Dropping is safe: the stream this report was for degrades to
			// unpaced delivery, not to a stall.
			std::cout << stamp()
			          << "reportPosition: table full, dropping report"
			          << std::endl;
			return;
			}
		}
	web_positions_[key] = WebPosition{pos, playing, now};
	}

float GainDrive::web_position_lookup(const std::string& user,
                                     const std::string& token)
	{
	const auto now = std::chrono::steady_clock::now();
	std::lock_guard<std::mutex> lock(web_pos_mu_);
	auto it = web_positions_.find(user + "\n" + token);
	if (it == web_positions_.end()) return CAST_POS_BUFFERING;
	const auto age = now - it->second.at;
	if (age > POS_STALE) return CAST_POS_BUFFERING;
	// While playing the playhead has moved since the report; while paused it
	// has not.  The paused case is exact, so a pause of any length keeps the
	// stream held at the lead instead of drifting ahead by the pause.
	if (!it->second.playing) return it->second.pos;
	return it->second.pos + std::chrono::duration<float>(age).count();
	}

void GainDrive::routes_stream()
	{
	// stream — serve audio file directly or transcode via ffmpeg.
	// In cast mode: redirect playback to the Chromecast and return 204 to the
	// calling client.  The Chromecast authenticates its own request with a
	// castToken query parameter instead of normal credentials.

	server_.Get("/rest/stream.view", [this](const httplib::Request& req,
	                                        httplib::Response& res) {
		auto it = req.params.find("id");
		if (it == req.params.end()) {
			res.set_content(subsonic_error(10, "Required parameter missing: id."),
			                "application/xml");
			return;
			}
		// The id is read before the token is checked, because the token is
		// scoped to one song: it authorises *this* id or nothing.
		const int req_song_id = to_int(it->second, -1);
		auto tok_it = req.params.find("castToken");
		bool cast_authed = tok_it != req.params.end()
		                && cast_manager_.valid_token(tok_it->second, req_song_id);
		// The browser-driven cast's grant travels on the same parameter, so a
		// URL looks the same whichever cast built it — but it is a *different*
		// credential and must not collapse into cast_authed. Everything below
		// that keys on cast_authed is about the server's own session: the
		// offset the LOAD declared, and the receiver's habit of probing with
		// timeOffset stripped. A grant has no session behind it, so none of
		// that applies; it is an ordinary client request that happens to carry
		// a token where a password would be. What it does carry that the
		// session token cannot is an account, which is why the bitrate ceiling
		// below still finds one to apply.
		std::string grant_user;
		if (!cast_authed && tok_it != req.params.end())
			grant_user = stream_grant_user(tok_it->second, req_song_id);
		const bool grant_authed = !grant_user.empty();

		if (!cast_authed && !grant_authed && !check_auth(req, res, store_)) return;

		auto song = store_.get_song(req_song_id);
		if (!song) {
			res.set_content(subsonic_error(70, "Song not found."), "application/xml");
			return;
			}

		// A cast stream carries no user, so the token has to be the authority
		// for it — CastManager::valid_token() has already bound it to this
		// exact song id. For everyone else, another user's uploads are not
		// readable by id.
		// A grant skips it for the same reason and with the same safety: the
		// check was run against its account at getCastToken, and the grant
		// names the one song it passed for.
		if (!cast_authed && !grant_authed
		    && !check_item_read_perm(req, res, store_, uploads_root_name_,
		                             song->path, fmt_of(req) == "json")) return;

		// Compose-and-validate the absolute song path once. Streamer reads from
		// it (via std::ifstream and ffmpeg argv) — refuse anything outside
		// the configured roots before handing it off.
		std::string song_abs = store_.abs_path(song->path);
		if (!store_.path_is_within_root(song_abs)) {
			std::cout << stamp() << "stream: refusing path outside every root: "
			          << song_abs << std::endl;
			res.status = 403;
			return;
			}

		// If cast mode is active and the caller is not the Chromecast itself,
		// instruct the Chromecast to fetch the stream and return 204 here.
		//
		// Only for the client that *owns* the session. Without that test this
		// is a process-global redirect: every stream request in the server —
		// the phone, a third-party client, curl, a different account entirely
		// — was answered 204 and pushed onto whatever receiver anyone had most
		// recently picked. A client that sent no castController is never the
		// owner, so it simply plays locally, which is what every client that
		// does not drive the server's cast endpoints wants.
		//
		// The owner can decline it with castRedirect=false, and one caller
		// needs to: when the receiver has no screen it is sent the film's
		// *soundtrack*, and the web client keeps the picture, muted and in
		// step with it.  That request is not the client about to play the
		// track a second time — it is the other half of one playback — and
		// answering it 204 both loses the picture and, through the
		// cast_load_song() below, re-issues the LOAD as a side effect.
		if (cast_manager_.active() && !cast_authed
		    && req.get_param_value("castRedirect") != "false"
		    && cast_owned_by(req)) {
			// Native seek: the URL serves the full file, and the LOAD message
			// tells the receiver where to seek.  No timeOffset in the URL.
			auto to_it = req.params.find("timeOffset");
			float cast_offset = to_it != req.params.end()
			    ? to_float(to_it->second, 0.0f) : 0.0f;
			// No caption: a client redirected here never asked for one, and
			// the picker sends its choice through castLoad.
			cast_load_song(req, *song, to_int(it->second, -1), cast_offset, 0);
			res.status = 204;
			return;
			}

		auto qp = [&](const std::string& k, const std::string& def = "") {
			auto it2 = req.params.find(k);
			return it2 != req.params.end() ? it2->second : def;
			};

		int         max_bitrate = to_int(qp("maxBitRate"), 0);
		std::string format      = qp("format");
		// Reject a format we have no encoder for here, where the Subsonic error
		// helpers live.  Letting it reach ffmpeg produced a 200 with an empty
		// body, which every client reports as a corrupt file rather than as a
		// bad request.
		if (!format.empty() && format != "raw") {
			auto t = target_for(format);
			if (!t || t->encoder.empty()) {
				bool as_json = fmt_of(req) == "json";
				std::string msg = "Unsupported format: " + format + ".";
				res.set_content(as_json ? subsonic_error_json(10, msg.c_str())
				                        : subsonic_error(10, msg.c_str()),
				                as_json ? "application/json" : "application/xml");
				return;
				}
			}

		// Enforce the user account's max_bitrate as a ceiling (0 = unlimited).
		// Cast requests authenticate via token and have no 'u' param; skip for those.
		//
		// Skipped for video, and that exemption is load-bearing rather than a
		// policy preference.  Any non-zero max_bitrate sets `constrained` in
		// serve_video and disqualifies both the direct and remux tiers — while
		// nativeSeek is a pure function of the codec pair and never sees it.  A
		// capped account would therefore be told every video is Range-seekable
		// and handed a chunked stream with Accept-Ranges: none, and would have
		// H.264/AAC MP4s re-encoded that could have been served off disk
		// untouched.  Subsonic defines maxBitRate as an audio ceiling anyway,
		// and a client that really wants a smaller picture still says so with
		// size= or maxBitRate=, which constrains exactly as before.
		//
		// An audio-only request is *not* a video request — it produces an
		// ordinary audio transcode through the ordinary audio path — so the
		// ceiling applies to it exactly as it does to a music track.  That is
		// why the format is parsed above rather than below: the ceiling now
		// depends on it.
		bool audio_only = audio_only_request(song->is_video, format,
		                                     song->audio_codec);
		//
		// A *grant* does not escape it, which is where the two tokens part
		// company. The session token skips this because there is genuinely no
		// account behind the request; a grant names one, so the person's
		// ceiling follows their music onto the receiver exactly as it follows
		// it into their browser.
		if (!cast_authed && (!song->is_video || audio_only)) {
			int acct_max;
			if (grant_authed) {
				auto gu  = store_.get_user(grant_user);
				acct_max = gu ? gu->max_bitrate : 0;
				}
			else acct_max = request_max_bitrate(req, store_);
			if (acct_max > 0 && (max_bitrate == 0 || max_bitrate > acct_max))
				max_bitrate = acct_max;
			}
		// The Chromecast sometimes probes the stream URL with timeOffset stripped.
		// Always use the authoritative offset stored at castLoad time for cast
		// requests so the probe and the real request both start at the right position.
		int         time_offset = cast_authed
		    ? static_cast<int>(last_cast_offset_)
		    : to_int(qp("timeOffset"), 0);

		// Chromecast metadata probes arrive as Range requests against the stream URL.
		// For seeked streams (time_offset > 0) these would otherwise hit serve_transcoded
		// which returns chunked output with no Content-Length, making it impossible for
		// the receiver to read the format's duration/seek tables.  Force time_offset=0
		// so these Range requests are served directly from the raw file.
		if (cast_authed && !req.get_header_value("Range").empty() && time_offset > 0)
			time_offset = 0;

		// Seeked-stream probe: the Chromecast strips timeOffset from its probe
		// request.  We must not return a full transcoded stream (blasts megabytes
		// at LAN speed while throttle is suppressed during BUFFERING), but we also
		// must not return an empty body (Content-Length: 0 makes the receiver treat
		// the track as finished and abort the real seeked request).  Serve a small
		// slice of the raw file from t=0 — enough for the receiver to validate the
		// URL, well within the pre-buffer window (prebuf ≈ 30 s of audio).
		if (cast_authed
		        && req.params.find("timeOffset") == req.params.end()
		        && last_cast_offset_ > 0.0f
		        && req.get_header_value("Range").empty()) {
			std::cout << stamp() << "cast probe: id=" << log_safe(it->second, 64)
			          << " offset=" << last_cast_offset_ << std::endl;
			auto probe_si = streamer_song(*song, song_abs,
			                              std::min(song->file_size,
			                                       (int64_t)32768));
			// No VideoOptions, deliberately rather than by omission: this call
			// stops before it, so the one path that must never honour a client's
			// container declaration gets the empty set by construction.
			Streamer::serve(req, res, probe_si, transcode_cache_, 0, "", 0, true, {});
			return;
			}

		if (cast_authed) {
			// Log Range header so we can see what the Cast receiver is requesting.
			auto range = req.get_header_value("Range");
			std::cout << stamp() << "cast stream: id=" << log_safe(it->second, 64)
			          << " size=" << song->file_size
			          << " range=[" << (range.empty() ? "none" : range) << "]"
			          << std::endl;
			}

		auto si = streamer_song(*song, song_abs);

		// For Cast streams, pass a callback that returns the receiver's current
		// playback position from the cached status (updated every ~0.5 s by the
		// web client's poll).  The streamer uses this to keep the buffer at a
		// stable level without relying on any device-specific buffer size.
		std::function<float()> get_pos;
		if (cast_authed) {
			int gen = cast_manager_.load_generation();
			get_pos = [this, gen]{
				// Return -2 when a newer stream has started — the streamer treats
				// this as a stop signal so the old thread exits promptly.
				if (cast_manager_.load_generation() != gen) return CAST_POS_STOP;
				auto s = cast_manager_.get_status();
				// BUFFERING means "seeking to this position", not "played up to here".
				// Return CAST_POS_BUFFERING to suppress throttle until playback starts.
				if (s.player_state != "PLAYING") return CAST_POS_BUFFERING;
				return s.current_time;
				};
			}
		else if (!grant_authed && !song->is_video) {
			// The web player names a pacing session with posToken and feeds it
			// through reportPosition, so the stream is throttled against the
			// real playhead by the same position branch a cast stream uses.
			// Audio only: the video ladder deliberately does not pace (see
			// serve_video), and a grant's player cannot report.  The reported
			// currentTime is relative to the served stream, which is the frame
			// both throttle branches account in: a timeOffset stream starts
			// at the seek point on both sides.
			auto pt = req.params.find("posToken");
			if (pt != req.params.end() && !pt->second.empty()
			    && pt->second.size() <= 64) {
				get_pos = [this, user = req.get_param_value("u"),
				           token = pt->second]{
					return web_position_lookup(user, token);
					};
				}
			}

		if (!cast_authed)
			std::cout << stamp() << "stream: id=" << log_safe(it->second, 64)
			          << " codec=" << song->codec
			          << " size=" << song->file_size
			          << " duration=" << song->duration
			          << (time_offset > 0 ? " offset=" + std::to_string(time_offset) : "")
			          << (!format.empty() ? " fmt=" + format : "")
			          << std::endl;
		bool estimate_length = qp("estimateContentLength") == "true";
		// Video-only per the spec, and ignored for audio by Streamer::serve().
		// `duration` is what makes an HLS segment a segment: hls.m3u8 points
		// every segment back here with a timeOffset and a length.
		// Validated here rather than in Streamer, so the one caller that can be
		// reached from outside is the one that checks. Anything unparseable
		// becomes empty, which means "do not scale" — the same as omitting it.
		VideoOptions vopts{
			.size             = sane_video_size(qp("size")),
			.segment_duration = to_int(qp("duration"), 0),
			// A receiver fetches for itself, and a stream with no
			// Content-Length is the one thing it must not be handed after a
			// LOAD announced video/mp4.  The literal "true" only, as pace and
			// estimateContentLength read it.
			.start_immediately = !cast_authed
			                  && qp("startImmediately") == "true",
			};
		// What the client can be sent untouched.  Never for a cast token, and
		// that is not caution.  A server-driven cast URL is fetched by the
		// *receiver*, which declared none of this, and the LOAD it is playing
		// announced a contentType that cast_mime_for() computed before a byte
		// was served.  Honouring a capability the controller claimed would send
		// Matroska to a receiver told video/mp4, which refuses the media
		// outright and reads as a broken file.  The audio half is no safer: the
		// same LOAD named an audio type the soundtrack transcode was going to
		// produce.
		const Playable playable = cast_authed
		    ? Playable{}
		    : parse_playable(qp("playable"));
		Streamer::serve(req, res, si, transcode_cache_, max_bitrate, format,
		                time_offset, cast_authed, std::move(get_pos),
		                estimate_length, vopts, playable);
		});

	// reportPosition, a gaindrive extension.  The web player's playhead for
	// one stream, named by the posToken its stream.view URL carried.  The
	// pacer throttles against it exactly as a cast stream is throttled
	// against the receiver's status; see serve_direct() in streamer.cc.
	server_.Get("/rest/reportPosition.view", [this](const httplib::Request& req,
	                                                httplib::Response& res) {
		if (!check_auth(req, res, store_)) return;
		bool use_json = (fmt_of(req) == "json");
		auto qp = [&](const char* k) {
			auto i = req.params.find(k);
			return i == req.params.end() ? std::string() : i->second;
			};
		const std::string token = qp("token");
		const std::string pos_s = qp("pos");
		// The size test is a bound, not a format: the token is a map key the
		// client chooses freely.
		if (token.empty() || token.size() > 64 || pos_s.empty()) {
			res.set_content(use_json
			    ? subsonic_error_json(10, "Required parameter missing: token/pos.")
			    : subsonic_error(10, "Required parameter missing: token/pos."),
			    use_json ? "application/json" : "application/xml");
			return;
			}
		web_position_report(qp("u"), token,
		                    std::strtof(pos_s.c_str(), nullptr),
		                    qp("playing") == "true");
		res.set_content(use_json ? subsonic_ok_json() : subsonic_ok(),
		                use_json ? "application/json" : "application/xml");
		});

	// download — the original file, never transcoded and never bitrate-capped.
	// The per-user max_bitrate is deliberately not consulted: "download" is
	// defined by the API as the original media data, and a capped download
	// would silently hand the user a different file than the one they asked
	// for.  Only song ids are supported; zipping a folder is out of scope.
	server_.Get("/rest/download.view", [this](const httplib::Request& req,
	                                           httplib::Response& res) {
		if (!check_auth(req, res, store_)) return;
		bool use_json = (fmt_of(req) == "json");
		auto err = [&](int code, const char* msg) {
			res.set_content(use_json ? subsonic_error_json(code, msg)
			                         : subsonic_error(code, msg),
			                use_json ? "application/json" : "application/xml");
			};

		auto it = req.params.find("id");
		if (it == req.params.end()) {
			err(10, "Required parameter missing: id."); return;
			}
		auto song = store_.get_song(to_int(it->second, -1));
		if (!song) { err(70, "Song not found."); return; }
		if (!check_item_read_perm(req, res, store_, uploads_root_name_,
		                          song->path, use_json)) return;

		std::string song_abs = store_.abs_path(song->path);
		if (!store_.path_is_within_root(song_abs)) {
			std::cout << stamp() << "download: refusing path outside every root: "
			          << song_abs << std::endl;
			res.status = 403;
			return;
			}

		std::string name = std::filesystem::path(song->path).filename().string();
		// Two spellings of the filename: a sanitised ASCII one for clients that
		// only read the bare parameter, and the RFC 5987 form for the rest.
		// A quote or newline left in the ASCII form would let a filename break
		// out of the header.
		std::string ascii;
		for (unsigned char c : name)
			ascii += (c < 0x20 || c == 0x7f || c == '"' || c == '\\'
			          || c >= 0x80) ? '_' : static_cast<char>(c);
		std::ostringstream enc;
		enc << std::hex << std::uppercase << std::setfill('0');
		for (unsigned char c : name) {
			if (isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~')
				enc << static_cast<char>(c);
			else
				enc << '%' << std::setw(2) << static_cast<int>(c);
			}
		res.set_header("Content-Disposition",
		               "attachment; filename=\"" + ascii + "\"; "
		               "filename*=UTF-8''" + enc.str());

		std::cout << stamp() << "download: id=" << log_safe(it->second, 64)
		          << " path=" << song->path
		          << " size=" << song->file_size << std::endl;

		auto si = streamer_song(*song, song_abs);
		Streamer::serve_raw(req, res, si);
		});
	// getVideos — every video in the library, as Child entries.  Videos share
	// the songs table with audio, so this is the ordinary song serialiser with
	// a different envelope key; isVideo and type are derived from the codec.

	server_.Get("/rest/getVideos.view", [this](const httplib::Request& req,
	                                            httplib::Response& res) {
		if (!check_auth(req, res, store_)) return;
		bool use_json = (fmt_of(req) == "json");
		auto videos   = store_.get_videos();
		int  mbr      = request_max_bitrate(req, store_);

		std::string body;
		if (use_json)
			body = subsonic_ok_json([&videos, mbr](nlohmann::json& r) {
				nlohmann::json entries = nlohmann::json::array();
				for (auto& v : videos)
					entries.push_back(song_entry_json(v, mbr));
				r["videos"] = {{ "video", entries }};
				});
		else
			body = subsonic_ok([&videos, mbr](XMLDocument& doc, XMLElement* root) {
				auto* el = doc.NewElement("videos");
				for (auto& v : videos)
					el->InsertEndChild(song_entry_xml(doc, v, "video", mbr));
				root->InsertEndChild(el);
				});
		res.set_content(body, use_json ? "application/json" : "application/xml");
		});

	// getVideoInfo — the subtitle and audio tracks inside one video file.
	// Runs ffprobe per call rather than caching: it is a per-playback lookup,
	// not a browse path, and a stale track list is worse than a slow one.
	// No <conversion> child is emitted — nothing pre-transcodes today, and
	// advertising a conversion that does not exist is worse than silence.
	server_.Get("/rest/getVideoInfo.view", [this](const httplib::Request& req,
	                                               httplib::Response& res) {
		if (!check_auth(req, res, store_)) return;
		bool use_json = (fmt_of(req) == "json");
		auto err = [&](int code, const char* msg) {
			res.set_content(use_json ? subsonic_error_json(code, msg)
			                         : subsonic_error(code, msg),
			                use_json ? "application/json" : "application/xml");
			};

		auto it = req.params.find("id");
		if (it == req.params.end()) {
			err(10, "Required parameter missing: id."); return;
			}
		int  song_id = to_int(it->second, -1);
		auto song    = store_.get_song(song_id);
		if (!song || !song->is_video) { err(70, "Video not found."); return; }
		if (!check_item_read_perm(req, res, store_, uploads_root_name_,
		                          song->path, use_json)) return;

		auto streams = store_.get_video_streams(song_id);

		std::string body;
		if (use_json)
			body = subsonic_ok_json([&](nlohmann::json& r) {
				nlohmann::json caps = nlohmann::json::array();
				for (auto& c : streams.captions)
					caps.push_back({ {"id",   sid(c.index)},
					                 {"name", c.title.empty() ? c.language
					                                          : c.title},
					                 {"source", caption_source(c)} });
				nlohmann::json tracks = nlohmann::json::array();
				for (auto& a : streams.audio_tracks)
					tracks.push_back({ {"id",           sid(a.index)},
					                   {"name",         a.title},
					                   {"languageCode", a.language} });
				r["videoInfo"] = {
					{"id",         sid(song_id)},
					{"captions",   caps},
					{"audioTrack", tracks}
					};
				});
		else
			body = subsonic_ok([&](XMLDocument& doc, XMLElement* root) {
				auto* vi = doc.NewElement("videoInfo");
				vi->SetAttribute("id", song_id);
				for (auto& c : streams.captions) {
					auto* el = doc.NewElement("captions");
					el->SetAttribute("id",   c.index);
					el->SetAttribute("name",
						(c.title.empty() ? c.language : c.title).c_str());
					el->SetAttribute("source", caption_source(c));
					vi->InsertEndChild(el);
					}
				for (auto& a : streams.audio_tracks) {
					auto* el = doc.NewElement("audioTrack");
					el->SetAttribute("id",           a.index);
					el->SetAttribute("name",         a.title.c_str());
					el->SetAttribute("languageCode", a.language.c_str());
					vi->InsertEndChild(el);
					}
				root->InsertEndChild(vi);
				});
		res.set_content(body, use_json ? "application/json" : "application/xml");
		});

	// getCaptions — WebVTT for one subtitle track.  Returns the file itself,
	// not a Subsonic envelope, which is what the spec asks for.  `format` is
	// accepted and ignored: WebVTT is what a <track> element can consume, and
	// handing back SRT would only push the conversion onto the client.
	server_.Get("/rest/getCaptions.view", [this](const httplib::Request& req,
	                                              httplib::Response& res) {
		bool use_json = (fmt_of(req) == "json");

		auto it = req.params.find("id");
		if (it == req.params.end()) {
			res.set_content(use_json
			                ? subsonic_error_json(10, "Required parameter missing: id.")
			                : subsonic_error(10, "Required parameter missing: id."),
			                use_json ? "application/json" : "application/xml");
			return;
			}
		// captionId selects an embedded stream; absent means the sidecar file.
		auto cid_it = req.params.find("captionId");
		int  index  = cid_it != req.params.end()
		    ? to_int(cid_it->second, -1) : -1;

		// The Chromecast fetches its own subtitle track and has no credentials
		// to do it with — the same problem stream.view solves the same way.
		// Handing the television the account's password instead would work,
		// and is what the phone apps used to do for the URLs they built
		// themselves; getCastToken is what stopped that, and the second
		// validator below is its half of this endpoint.
		//
		// Both ids are read before the token is checked, for the same reason
		// stream.view reads its id first: the token authorises one song and one
		// of the caption ids that song's LOAD declared, so there is nothing to
		// check it against until both are known.
		const int req_song_id = to_int(it->second, -1);
		auto tok_it = req.params.find("castToken");
		bool cast_authed = tok_it != req.params.end()
		                && cast_manager_.valid_caption_token(tok_it->second,
		                                                     req_song_id, index);
		// A grant is the other credential this parameter carries, and it is
		// scoped one notch wider: any caption of its song, rather than the
		// ids one LOAD declared. There is no LOAD here to mirror — the client
		// that minted it builds its own — and a subtitle of a song the account
		// may already read is not a wider reach than the song was.
		const bool grant_authed = !cast_authed && tok_it != req.params.end()
		                       && grant_allows_captions(tok_it->second,
		                                                req_song_id);
		if (!cast_authed && !grant_authed && !check_auth(req, res, store_)) return;

		// As in stream.view: either token is its own authority, everyone
		// else may not read another user's uploads by id.
		if (!cast_authed && !grant_authed) {
			auto song = store_.get_song(req_song_id);
			if (song && !check_item_read_perm(req, res, store_, uploads_root_name_,
			                                  song->path, use_json)) return;
			}

		auto vtt = store_.get_captions_vtt(to_int(it->second, -1), index);
		if (vtt.empty()) {
			std::cout << stamp() << "getCaptions: nothing for id=" << log_safe(it->second, 64)
			          << " captionId=" << index << std::endl;
			res.status = 404;
			return;
			}
		// Success is logged too, and only here does it matter who asked: a
		// Chromecast fetches its own subtitle track, so this line arriving from
		// the television's address is the proof that the receiver accepted the
		// tracks the LOAD declared and went looking for one. Its absence is the
		// single most useful fact when captions do not appear on a cast.
		std::cout << stamp() << "getCaptions: id=" << log_safe(it->second, 64)
		          << " captionId=" << index << " " << vtt.size() << " bytes"
		          << (cast_authed ? " (cast token)"
		                          : grant_authed ? " (grant)" : "")
		          << " to " << client_addr(req) << std::endl;
		res.set_content(vtt, "text/vtt");
		});

	// getChapters / saveChapters — the song markers inside one video.
	//
	// A full concert is one file, and these are what let a client say where
	// each song starts.  They live in a sidecar `<stem>.chapters.txt` beside
	// the video, never inside the container: MP4 chapters are a track within
	// the file, so writing one would be an `ffmpeg -c copy` rewrite of every
	// byte of a multi-gigabyte concert on every save.  Nothing about them is
	// stored in either database — the file on disk is the only copy, which is
	// what keeps the music DB a cache and relocate_prefix() unchanged.
	//
	// `name` is reported exactly as the file holds it, empty included.  A
	// client draws its own "Chapter 3" placeholder for a bare marker; filling
	// one in here would look harmless and is not, because a client that saved
	// what it read would write the placeholder into a line somebody had
	// deliberately left blank, and the next save would find it real.
	server_.Get("/rest/getChapters.view", [this](const httplib::Request& req,
	                                              httplib::Response& res) {
		if (!check_auth(req, res, store_)) return;
		bool use_json = (fmt_of(req) == "json");
		auto err = [&](int code, const char* msg) {
			res.set_content(use_json ? subsonic_error_json(code, msg)
			                         : subsonic_error(code, msg),
			                use_json ? "application/json" : "application/xml");
			};

		auto it = req.params.find("id");
		if (it == req.params.end()) {
			err(10, "Required parameter missing: id."); return;
			}
		int  song_id = to_int(it->second, -1);
		auto song    = store_.get_song(song_id);
		if (!song) { err(70, "Song not found."); return; }
		if (!check_item_read_perm(req, res, store_, uploads_root_name_,
		                          song->path, use_json)) return;

		auto vc = store_.get_chapters(song_id);
		bool writable = item_write_allowed(req, store_, uploads_root_name_,
		                                   song->path);
		write_chapters_response(res, song_id, vc, song->duration, writable,
		                        use_json);
		});

	// getAlbumChapters — every chaptered video in one album folder.
	//
	// A second endpoint rather than an albumId mode on getChapters, because
	// the two read different things and the difference is the point:
	// getChapters reads the sidecar and is therefore always right, which is
	// what a playback path needs; this reads the `chapters` index the scan
	// maintains, which is what a browse path needs. Listing a concert's songs
	// must not cost a file read per video — or, for a rip with no sidecar, an
	// ffprobe.
	//
	// The consequence, stated because it looks like a bug: a video whose
	// markers live only in its container appears in the panel and *not* here,
	// until somebody saves them, which writes the sidecar the scan indexes.
	server_.Get("/rest/getAlbumChapters.view", [this](const httplib::Request& req,
	                                                   httplib::Response& res) {
		if (!check_auth(req, res, store_)) return;
		bool use_json = (fmt_of(req) == "json");
		auto err = [&](int code, const char* msg) {
			res.set_content(use_json ? subsonic_error_json(code, msg)
			                         : subsonic_error(code, msg),
			                use_json ? "application/json" : "application/xml");
			};

		auto it = req.params.find("id");
		if (it == req.params.end()) {
			err(10, "Required parameter missing: id."); return;
			}
		int folder_id = to_int(it->second, -1);

		std::string folder_rel = store_.get_folder_path(folder_id);
		if (folder_rel.empty()) {
			// Logged, unlike most 70s, because of what the client does with it:
			// the album view swallows a failure here and simply draws its tracks
			// without chapters, which is indistinguishable from a film that has
			// none. A folder id that no longer resolves is the likely way that
			// happens to an album whose markers were listed a moment ago --
			// INSERT OR REPLACE hands out fresh rowids, so a rescan can strand
			// the id a client is holding.
			std::cout << stamp() << "getAlbumChapters: no folder with id "
			          << folder_id << "; the album view will list this one"
			             " without its chapters" << std::endl;
			err(70, "Album folder not found."); return;
			}
		if (!check_item_read_perm(req, res, store_, uploads_root_name_,
		                          folder_rel, use_json)) return;

		auto vids = store_.get_album_chapters(folder_id);

		std::string body;
		if (use_json)
			body = subsonic_ok_json([&](nlohmann::json& r) {
				nlohmann::json list = nlohmann::json::array();
				for (const auto& v : vids)
					list.push_back({
						{"id",      sid(v.song_id)},
						{"title",   v.title},
						{"chapter", chapter_array_json(v.chapters, v.duration)}
						});
				r["albumChapters"] = {
					{"id",   sid(folder_id)},
					{"song", list}
					};
				});
		else
			body = subsonic_ok([&](XMLDocument& doc, XMLElement* root) {
				auto* parent = doc.NewElement("albumChapters");
				parent->SetAttribute("id", folder_id);
				for (const auto& v : vids) {
					auto* el = doc.NewElement("song");
					el->SetAttribute("id",    v.song_id);
					el->SetAttribute("title", v.title.c_str());
					chapter_array_xml(doc, el, v.chapters, v.duration);
					parent->InsertEndChild(el);
					}
				root->InsertEndChild(parent);
				});
		res.set_content(body, use_json ? "application/json" : "application/xml");
		});

	// The body is the chapter file itself, as text/plain, and it goes through
	// the same parse_chapters() that reads one off disk.  That is one
	// definition of the format rather than two, and it makes the round trip
	// trivially a fixed point — but it is also the only shape that works here.
	// Repeated start=/name= parameters cannot be used:
	//
	//  * httplib's parse_query_text() keeps a set of each whole "key=value"
	//    token and silently drops an exact repeat, so two chapters both called
	//    "Encore" would lose one name= and rename every marker after it.
	//  * CPPHTTPLIB_FORM_URL_ENCODED_PAYLOAD_MAX_LENGTH is 8192 for urlencoded
	//    bodies specifically, and set_payload_max_length() does not raise it.
	//
	// Both are undocumented behaviour of a pinned vendored copy, which is the
	// failure class third_party/README.md pins that copy to avoid.
	//
	// An empty body writes an empty file rather than removing it.  That empty
	// file is a tombstone: without it, clearing the markers of a rip whose
	// container carries its own would make the container's list come back, and
	// there would be no way to say "this film has no chapters" at all.
	server_.Post("/rest/saveChapters.view", [this](const httplib::Request& req,
	                                                httplib::Response& res) {
		if (!check_auth(req, res, store_)) return;
		bool use_json = (fmt_of(req) == "json");
		auto err = [&](int code, const char* msg) {
			res.set_content(use_json ? subsonic_error_json(code, msg)
			                         : subsonic_error(code, msg),
			                use_json ? "application/json" : "application/xml");
			};

		auto it = req.params.find("id");
		if (it == req.params.end()) {
			err(10, "Required parameter missing: id."); return;
			}
		int  song_id = to_int(it->second, -1);
		auto song    = store_.get_song(song_id);
		if (!song) { err(70, "Song not found."); return; }
		if (!check_item_write_perm(req, res, store_, uploads_root_name_,
		                           song->path, use_json)) return;

		auto parsed = parse_chapters(req.body);
		if (parsed.chapters.size() > MediaStore::MAX_CHAPTERS) {
			err(0, "Too many chapters."); return;
			}

		// utf8_clean is not the same job as the control-character strip
		// parse_chapters already did: that one keeps the file's line format
		// intact, this one keeps invalid UTF-8 out of a JSON document, where
		// dump() throws.  Both are needed, and this is the boundary the other
		// arbitrary strings are cleaned at.
		for (auto& c : parsed.chapters)
			c.name = utf8_clean(c.name, MAX_CHAPTER_NAME_BYTES);

		if (!store_.save_chapters(song_id, parsed.chapters)) {
			err(0, "Could not write the chapter file."); return;
			}
		std::cout << stamp() << "saveChapters: id=" << song_id << " "
		          << parsed.chapters.size() << " marker(s), "
		          << parsed.skipped << " line(s) skipped, from "
		          << client_addr(req) << std::endl;

		// Re-read rather than echo: the server sorts, sanitises and may skip
		// lines, so the client must adopt what was actually written or the
		// panel and the file disagree until the next load.  Same rule as
		// castLoad's reply reporting the decision instead of letting the
		// client re-derive it.
		auto vc = store_.get_chapters(song_id);
		write_chapters_response(res, song_id, vc, song->duration, true,
		                        use_json);
		});

	// hls.m3u8 — a playlist computed from the stored duration.  Deliberately
	// stateless: no segment directory, no session, no temp files.  Every
	// segment URL is an ordinary stream.view transcode bounded by timeOffset
	// and duration, which is exactly how Subsonic does it.  Nothing here needs
	// cleaning up if a client walks away mid-playlist.
	//
	// Registered at two paths.  `hls.m3u8` is the spec's spelling and the one
	// the Android and iOS clients build, because ExoPlayer and AVFoundation
	// infer HLS from that extension; `hls.view` is what a client composing
	// every URL as <name>.view asks for, and it reached the "Not implemented"
	// catch-all before.  One handler can serve both only because every URI in
	// the body is *relative* — a segment resolves against /rest/ whichever
	// path was fetched — and the one place that is not true, a variant URI
	// naming this endpoint again, spells itself from req.path.
	auto hls_handler = [this](const httplib::Request& req,
	                           httplib::Response& res) {
		if (!check_auth(req, res, store_)) return;
		const bool use_json = (fmt_of(req) == "json");
		auto err = [&](int code, const char* msg) {
			res.set_content(use_json ? subsonic_error_json(code, msg)
			                         : subsonic_error(code, msg),
			                use_json ? "application/json" : "application/xml");
			};

		auto it = req.params.find("id");
		if (it == req.params.end()) {
			err(10, "Required parameter missing: id.");
			return;
			}
		auto song = store_.get_song(to_int(it->second, -1));
		if (!song || !song->is_video) {
			err(70, "Video not found.");
			return;
			}
		if (!check_item_read_perm(req, res, store_, uploads_root_name_,
		                          song->path, use_json)) return;

		auto qp = [&](const std::string& k, const std::string& def = "") {
			auto it2 = req.params.find(k);
			return it2 != req.params.end() ? it2->second : def;
			};
		// bitRate is the spec's spelling here; it may carry an "@WxH" suffix
		// (e.g. "1000@640x480") naming the frame size for that variant, and
		// the spec allows it more than once, which is the request for a master
		// playlist.  Reading it with params.find() was wrong on both counts:
		// std::multimap does not promise *which* of several equal keys it
		// hands back, so the answer to a repeated bitRate was unspecified.
		//
		// Both halves are normalised through the same validators stream.view
		// uses, and not merely escaped. They are written into the playlist
		// *body* — a variant's URI and its RESOLUTION attribute as much as a
		// segment's query string — which is not a URL and not XML, so nothing
		// downstream would have caught a newline in either: a forged "#EXT-X-"
		// line, or a second URL of the sender's choosing. Round-tripping
		// through to_int/sane_video_size means only a number and a WxH can
		// ever be emitted, whatever arrived.
		struct Variant { int kbps; std::string size; };
		std::vector<Variant> variants;
		auto [blo, bhi] = req.params.equal_range("bitRate");
		for (auto b = blo; b != bhi; ++b) {
			std::string kb = b->second, size;
			if (auto at = kb.find('@'); at != std::string::npos) {
				size = kb.substr(at + 1);
				kb   = kb.substr(0, at);
				}
			const Variant v{ to_int(kb, 0), sane_video_size(size) };
			// Nothing to say: neither half survived validation.  Note a bare
			// "bitRate=0@640x480" does survive — 0 is the spec's "no limit",
			// and the frame size still governs.
			if (v.kbps <= 0 && v.size.empty()) continue;
			if (std::none_of(variants.begin(), variants.end(),
			                 [&](const Variant& o) {
			                 	return o.kbps == v.kbps && o.size == v.size;
			                 	}))
				variants.push_back(v);
			}

		const int SEGMENT = 10;
		int total = static_cast<int>(song->duration);
		if (total <= 0) {
			err(70, "Video has no known duration.");
			return;
			}

		// Credentials ride along on every URL in this body: the player fetches
		// the segments — and any variant playlist — itself, and carries none
		// of this request's context.  Only the parameters actually present are
		// echoed — an empty p= alongside t=/s= would send check_auth down the
		// password branch with a blank password and fail every segment.
		std::string auth;
		for (const char* k : { "u", "p", "t", "s", "c" }) {
			auto v = qp(k);
			if (!v.empty())
				auth += "&" + std::string(k) + "=" + url_encode(v);
			}
		auth += "&v=" + std::string(SUBSONIC_VER);

		// A master playlist announces each variant's BANDWIDTH, so only a
		// variant that named a bitrate can be one; a frame size alone still
		// governs a media playlist, as it always did.
		std::vector<Variant> announced;
		for (const auto& v : variants)
			if (v.kbps > 0) announced.push_back(v);
		// A total order rather than one on kbps alone: two variants may name
		// the same bitrate at different frame sizes, and std::sort is not
		// stable, so ordering on the bitrate alone would let two identical
		// requests produce two different playlists.
		std::sort(announced.begin(), announced.end(),
		          [](const Variant& a, const Variant& c) {
		          	return a.kbps != c.kbps ? a.kbps < c.kbps
		          	                        : a.size < c.size;
		          	});

		if (announced.size() >= 2) {
			// The variant URIs point back at this endpoint.  Stay on the
			// spelling the client used: req.path has already been through the
			// pre-routing handler, so a bare /rest/hls arrives here as
			// /rest/hls.view and self-references a path that is registered.
			const std::string self = req.path.substr(req.path.rfind('/') + 1);

			std::ostringstream mst;
			mst << "#EXTM3U\n"
			    << "#EXT-X-VERSION:3\n";
			for (const auto& v : announced) {
				// BANDWIDTH is honest for this encoder: video_ffmpeg_argv()
				// caps the picture at max(200, kbps-128) and adds a 128 kbps
				// AAC track on top.  PROGRAM-ID is gone from protocol version
				// 6, but is legal at the version 3 declared above and is what
				// Subsonic and Airsonic emit, so older players still get it.
				mst << "#EXT-X-STREAM-INF:PROGRAM-ID=1,BANDWIDTH="
				    << static_cast<long long>(v.kbps) * 1000;
				if (!v.size.empty()) mst << ",RESOLUTION=" << v.size;
				mst << "\n" << self << "?id=" << song->id
				    << "&bitRate=" << v.kbps;
				if (!v.size.empty()) mst << "@" << v.size;
				mst << auth << "\n";
				}
			// No #EXT-X-ENDLIST here: that tag terminates a media playlist,
			// and a master holds no segments to terminate.

			std::cout << stamp() << "hls: id=" << log_safe(it->second, 64)
			          << " variants=" << announced.size()
			          << " path=" << req.path << std::endl;
			res.set_header("Cache-Control", "no-store");
			res.set_content(mst.str(), "application/vnd.apple.mpegurl");
			return;
			}

		const std::string bitrate = (!variants.empty() && variants[0].kbps > 0)
		                          ? std::to_string(variants[0].kbps) : "";
		const std::string size    = variants.empty() ? "" : variants[0].size;

		std::ostringstream m3u;
		m3u << "#EXTM3U\n"
		    << "#EXT-X-VERSION:3\n"
		    << "#EXT-X-TARGETDURATION:" << SEGMENT << "\n"
		    << "#EXT-X-MEDIA-SEQUENCE:0\n"
		    << "#EXT-X-PLAYLIST-TYPE:VOD\n";
		// song->id rather than the raw id parameter: it is the same value once
		// resolved, and it is an int rather than whatever the client sent.
		for (int off = 0; off < total; off += SEGMENT) {
			int len = std::min(SEGMENT, total - off);
			m3u << "#EXTINF:" << len << ".0,\n"
			    << "stream.view?id=" << song->id
			    << "&timeOffset=" << off
			    << "&duration="   << len;
			if (!bitrate.empty()) m3u << "&maxBitRate=" << bitrate;
			if (!size.empty())    m3u << "&size=" << size;
			m3u << auth << "\n";
			}
		m3u << "#EXT-X-ENDLIST\n";

		std::cout << stamp() << "hls: id=" << log_safe(it->second, 64)
		          << " duration=" << total
		          << " segments=" << ((total + SEGMENT - 1) / SEGMENT)
		          << " path=" << req.path << std::endl;
		// This body contains the caller's credentials, once per segment, and
		// the player writes it to disk. It is also served under
		// Access-Control-Allow-Origin: * like everything else here. Nothing
		// should keep a copy of it.
		res.set_header("Cache-Control", "no-store");
		res.set_content(m3u.str(), "application/vnd.apple.mpegurl");
		};
	server_.Get("/rest/hls.m3u8", hls_handler);
	server_.Get("/rest/hls.view", hls_handler);
	}
