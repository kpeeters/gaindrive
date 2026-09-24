#include "gaindrive.hh"
#include "subsonic.hh"
#include "authz.hh"
#include "stamp.hh"
#include "apientry.hh"
#include "textutil.hh"
#include "netaddr.hh"
#include "codecs.hh"
#include "streamer.hh"
#include "wiimeq.hh"

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <mutex>
#include <thread>

#include <openssl/rand.h>

#include <iostream>

#include <tinyxml2.h>
#include <nlohmann/json.hpp>

using namespace tinyxml2;

// A comma-joined set, because there are only ever a handful of codec pairs and
// a table would be a schema change for a fact that is pure cache: worst case a
// forgotten refusal costs one failed LOAD again.  Neither codec name can
// contain a comma, so the join needs no escaping.
static const char* CAST_NOVIDEO_KEY = "cast_novideo:";


// Grace period before the SSE-as-heartbeat watchdog tears down a cast
// session whose listener has gone away. Long enough to ride out a page
// reload or a brief network blip; short enough that closing the tab
// actually stops the cast.
static constexpr int CAST_IDLE_GRACE_S = 30;

// How many SSE listeners one cast session may hold open at once. Each one is
// a worker thread parked for the life of the connection; see castEvents.
static constexpr int CAST_SSE_MAX_LISTENERS = 8;


// The receiver's volume as a client sees it: null until the first
// RECEIVER_STATUS carries one, so "not reported yet" and "level zero" stay
// distinguishable and a client keeps its buttons disabled rather than guess.
static nlohmann::json volume_json(const CastManager::VolumeState& v)
	{
	if (!v.known) return nullptr;
	return {{"level", v.level}, {"muted", v.muted}, {"fixed", v.fixed}};
	}

void GainDrive::routes_cast()
	{
	// getCastToken - a credential a receiver can fetch one track with, for a
	// cast this server is not driving.
	//
	// The Android and iOS apps hold their own Cast control channel and build
	// the receiver's URLs themselves, which until now meant building them with
	// `u`/`t`/`s`. getCaptions' own comment below says what is wrong with
	// that: it hands the television the account's password. This is the way
	// out, and the phone apps are its callers.
	//
	// **Gated on authentication alone** - not castRole, and not the
	// local-network rule the endpoints below carry. Neither would make sense
	// here: a client casting for itself is not asking this server to cast, so
	// neither the permission to drive this server's Chromecast nor the
	// question of which network this server is on has any bearing on it. It
	// sits above the gated block so the asymmetry is read rather than
	// discovered.
	//
	// What bounds it instead is the grant: one song, one account, twelve
	// hours, minted only after the same check_item_read_perm that castLoad
	// runs before minting its own. That check is the load-bearing one, because
	// what comes back opens URLs that need no credentials at all.
	//
	// It does widen something, deliberately: any account can now produce such
	// a URL, where before only a castRole one could. The
	// floor under it is that anyone who can read a song can already download
	// it.
	//
	// JSON only, the reason getServerSettings gives: this is a credential.

	server_.Get("/rest/getCastToken.view", [this](const httplib::Request& req,
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
			err(10, "Required parameter missing: id.");
			return;
			}
		const int song_id = to_int(it->second, -1);
		// get_song_entry() rather than get_song(): both carry the path the
		// permission check needs, and only this one carries cover_art_id -
		// which is what lets the grant cover the sleeve without the caller
		// naming it, and so without a caller being able to name someone
		// else's.
		auto song = store_.get_song_entry(song_id);
		if (!song) { err(70, "Song not found."); return; }
		if (!check_item_read_perm(req, res, store_, uploads_root_name_,
		                          song->path, use_json)) return;

		const std::string token = mint_stream_grant(req.get_param_value("u"),
		                                            song_id, song->cover_art_id);
		if (token.empty()) { err(0, "Could not mint a stream token."); return; }

		res.set_content(subsonic_ok_json([&](nlohmann::json& r) {
			r["castToken"] = token;
			}), "application/json");
		});

	// listCastDevices - return the cached device list and kick off a background
	// refresh so the next call will have up-to-date results.
	server_.Get("/rest/listCastDevices.view", [this](const httplib::Request& req,
	                                                  httplib::Response& res) {
		if (!check_auth(req, res, store_)) return;
		bool use_json = (fmt_of(req) == "json");
		if (!check_cast_perm(req, res, store_, use_json)) return;
		if (!check_cast_local(req, res, use_json)) return;

		auto devices = cast_manager_.cached_devices();
		cast_manager_.discover_background();

		// Read here rather than inside the two rendering arms so JSON and XML
		// cannot disagree about a device, and so the settings lookups happen
		// once each.
		std::vector<std::string> prefs;
		prefs.reserve(devices.size());
		for (auto& d : devices) {
			std::string p = store_.get_setting("cast_video:" + d.id);
			prefs.push_back(p.empty() ? "auto" : p);
			}

		std::string body;
		if (use_json) {
			body = subsonic_ok_json([&devices, &prefs](nlohmann::json& r) {
				nlohmann::json arr = nlohmann::json::array();
				for (size_t i = 0; i < devices.size(); i++) {
					auto& d = devices[i];
					arr.push_back({{"id", d.id}, {"name", d.name},
					               {"model", d.model},
					               {"address", d.address}, {"port", d.port},
					               {"manual", d.manual},
					               // What the device's `ca` TXT record said it
					               // can do; true when it announced nothing.
					               {"videoOut", d.video_out()},
					               // What a person decided about it, which
					               // overrides the record above.
					               {"videoPref", prefs[i]}});
					}
				r["castDevices"] = arr;
				});
			}
		else {
			body = subsonic_ok([&devices, &prefs](XMLDocument& doc,
			                                      XMLElement* root) {
				auto* el = doc.NewElement("castDevices");
				for (size_t i = 0; i < devices.size(); i++) {
					auto& d = devices[i];
					auto* dev = doc.NewElement("castDevice");
					dev->SetAttribute("id",      d.id.c_str());
					dev->SetAttribute("name",    d.name.c_str());
					dev->SetAttribute("model",   d.model.c_str());
					dev->SetAttribute("address", d.address.c_str());
					dev->SetAttribute("port",    d.port);
					dev->SetAttribute("manual",  d.manual);
					dev->SetAttribute("videoOut", d.video_out());
					dev->SetAttribute("videoPref", prefs[i].c_str());
					el->InsertEndChild(dev);
					}
				root->InsertEndChild(el);
				});
			}

		res.set_content(body, use_json ? "application/json" : "application/xml");
		});

	// setCastDevicePref - record what a person decided about one device.
	//
	// Server-side rather than in each client's storage because it is a fact
	// about the device, not about the viewer: whether a WiiM plays a video
	// file's sound is the same answer in every browser and on the phone.  The
	// per-viewer knob next to it in spirit, castSyncDelay, is client-side for
	// exactly the opposite reason - it is about the screen you are watching.
	//
	// Gated on castRole and deliberately not on admin, though the row is
	// process-global: the person standing in front of the device is the one
	// who knows whether it has a screen, and that is a cast user.  The write
	// is bounded - the id must name a device in cached_devices(), the value
	// is one of three enumerated strings, and the worst a cast user can do
	// with it is what the setting exists to do.  Decided during the security
	// pass, not overlooked.
	server_.Get("/rest/setCastDevicePref.view", [this](const httplib::Request& req,
	                                                   httplib::Response& res) {
		if (!check_auth(req, res, store_)) return;
		bool use_json = (fmt_of(req) == "json");
		if (!check_cast_perm(req, res, store_, use_json)) return;
		if (!check_cast_local(req, res, use_json)) return;

		auto err = [&](int code, const char* msg) {
			res.set_content(use_json ? subsonic_error_json(code, msg)
			                         : subsonic_error(code, msg),
			                use_json ? "application/json" : "application/xml");
			};

		std::string id   = req.get_param_value("deviceId");
		std::string pref = req.get_param_value("videoPref");
		if (id.empty())   { err(10, "Required parameter missing: deviceId."); return; }
		if (pref.empty()) { err(10, "Required parameter missing: videoPref."); return; }

		// Refused rather than stored, because an unrecognised value would be
		// written, read back as neither "send" nor "sound", and so behave as
		// `auto` with nothing anywhere saying the setting had not taken.
		if (pref != "auto" && pref != "send" && pref != "sound") {
			err(10, "videoPref must be auto, send or sound.");
			return;
			}

		// The id has to name a device we know about.  Not for authorisation -
		// there is nothing to authorise - but because the key is composed from
		// it, and an unvalidated one is an unbounded write into client.settings
		// by any account with cast permission.  Same bound ladder_size() puts
		// on the thumbnail table, for the same reason.
		auto devices = cast_manager_.cached_devices();
		bool known = false;
		for (auto& d : devices) if (d.id == id) { known = true; break; }
		if (!known) { err(70, "Cast device not found."); return; }

		// `auto` is stored as an empty value, which get_setting() cannot tell
		// from an absent row - which is the point: a device nobody has decided
		// about and one reset to auto read the same, so `auto` needs no
		// spelling of its own on the way back out.
		store_.set_setting("cast_video:" + id, pref == "auto" ? "" : pref);
		// Changing your mind about a device is the natural place to make it
		// reconsider, and the only one: a codec pair recorded as refused is
		// never re-tried otherwise, so new firmware - or a different device
		// that inherited the address, and with it the id - would be judged for
		// ever on what its predecessor could not play.
		store_.set_setting(CAST_NOVIDEO_KEY + id, "");
		std::cout << stamp() << "Cast: device " << id << " videoPref=" << pref
		          << " (any refused codecs forgotten)" << std::endl;

		res.set_content(use_json ? subsonic_ok_json() : subsonic_ok(),
		                use_json ? "application/json" : "application/xml");
		});

	// startCast - enter cast mode: subsequent stream requests go to the Chromecast.
	server_.Get("/rest/startCast.view", [this](const httplib::Request& req,
	                                            httplib::Response& res) {
		if (!check_auth(req, res, store_)) return;
		bool use_json = (fmt_of(req) == "json");
		if (!check_cast_perm(req, res, store_, use_json)) return;
		if (!check_cast_local(req, res, use_json)) return;

		auto it = req.params.find("id");
		if (it == req.params.end()) {
			res.set_content(
				use_json ? subsonic_error_json(10, "Required parameter missing: id.")
				         : subsonic_error(10, "Required parameter missing: id."),
				use_json ? "application/json" : "application/xml");
			return;
			}

		// Required, not optional with a fallback to `c=`. A client asking the
		// server to drive a Chromecast is already speaking the gaindrive
		// extension, so it can be asked to name itself - and refusing the
		// nameless case is what guarantees that "sent no castController" can
		// never own a session, and so can never collide with another client
		// that also sent none. A `c=` fallback would put every install of one
		// app under a single identity, which is the bug this endpoint is being
		// fixed for, one scale down.
		std::string controller = req.get_param_value("castController");
		if (controller.empty()) {
			const char* msg = "Required parameter missing: castController.";
			res.set_content(use_json ? subsonic_error_json(10, msg)
			                         : subsonic_error(10, msg),
			                use_json ? "application/json" : "application/xml");
			return;
			}

		auto devices = cast_manager_.cached_devices();
		CastManager::CastDevice chosen;
		bool found = false;
		for (auto& d : devices)
			if (d.id == it->second) { chosen = d; found = true; break; }

		if (!found) {
			res.set_content(
				use_json ? subsonic_error_json(70, "Cast device not found.")
				         : subsonic_error(70, "Cast device not found."),
				use_json ? "application/json" : "application/xml");
			return;
			}

		// There is one control channel, so a second owner claiming it displaces
		// the first rather than running beside it. Tearing the old session down
		// first is what sends the receiver a STOP and clears the previous
		// owner's song and offset; the generation bump inside cast_teardown()
		// is what drops that owner's castEvents connection, so its UI leaves
		// cast mode on its own. This is also the only way back in for a browser
		// that cleared its site data and lost the id it started the session
		// with.
		if (cast_manager_.active() && !cast_owned_by(req)) {
			std::cout << stamp() << "Cast: session taken over by user="
			          << req.get_param_value("u") << " device=" << chosen.name
			          << std::endl;
			cast_teardown();
			}

		cast_manager_.start(chosen);
		cast_claim(req.get_param_value("u"), controller);
		res.set_content(use_json ? subsonic_ok_json() : subsonic_ok(),
		                use_json ? "application/json" : "application/xml");
		});

	// stopCast - stop Chromecast playback and exit cast mode.
	server_.Get("/rest/stopCast.view", [this](const httplib::Request& req,
	                                           httplib::Response& res) {
		if (!check_auth(req, res, store_)) return;
		bool use_json = (fmt_of(req) == "json");
		if (!check_cast_perm(req, res, store_, use_json)) return;
		// A non-owner stops nothing and is told it succeeded. Deliberately not
		// an error: a client that has just been displaced by a takeover runs
		// its own cleanup path, and that must neither kill the session that
		// replaced it nor raise a dialog about a session it no longer has.
		if (cast_owned_by(req)) cast_teardown();
		res.set_content(use_json ? subsonic_ok_json() : subsonic_ok(),
		                use_json ? "application/json" : "application/xml");
		});

	// castEvents - SSE stream that pushes MEDIA_STATUS updates to the browser.
	// Each event is a JSON object with playerState, currentTime, duration.
	// The connection is kept alive by the Chromecast heartbeat; a 15-second
	// keepalive comment is sent if no real update arrives in that window.
	//
	// This connection also acts as the cast session's heartbeat: when the
	// browser disconnects (tab closed, network drop, OS sleep) and no new
	// listener reconnects within CAST_IDLE_GRACE_S seconds, the watchdog
	// armed in the RAII guard's destructor tears the cast session down.
	server_.Get("/rest/castEvents.view", [this](const httplib::Request& req,
	                                            httplib::Response& res) {
		if (!check_auth(req, res, store_)) return;
		{
		bool use_json = (fmt_of(req) == "json");
		if (!check_cast_perm(req, res, store_, use_json)) return;
		if (!check_cast_local(req, res, use_json)) return;
		}
		if (!cast_manager_.active() || !cast_owned_by(req)) {
			res.status = 204;
			return;
			}
		// The session this connection belongs to. A takeover bumps it, which is
		// how a displaced listener learns it has been replaced: `active_` is
		// true either side of the stop()/start() pair, and this thread spends
		// most of its life blocked inside wait_status(), so the flag alone
		// would never show the gap.
		const int session_gen = cast_session_gen_.load();

		// A cap on concurrent SSE listeners. Each one parks a worker thread
		// in wait_status() for the life of the connection, so without a
		// ceiling the session's one authorised owner could occupy the whole
		// pool with a loop over EventSource(). A real client holds one; the
		// allowance covers a reload race and a second tab.
		if (cast_sse_listeners_.load() >= CAST_SSE_MAX_LISTENERS) {
			std::cout << stamp() << "Cast: refusing SSE listener, "
			          << CAST_SSE_MAX_LISTENERS << " already attached"
			          << std::endl;
			res.status = 503;
			return;
			}

		res.set_header("Cache-Control",    "no-cache");
		res.set_header("X-Accel-Buffering","no");   // disable nginx/apache buffering

		// RAII helper: increments the listener counter on construction and
		// arms the auto-stop watchdog on destruction.  Captured via
		// shared_ptr because httplib stores the content provider in a
		// std::function (which requires copyable callables); the guard's
		// destructor still fires exactly once, when the last copy of the
		// lambda is dropped - i.e. when the connection ends, regardless of
		// whether it ended via sink.write returning false (client gone),
		// wait_status seeing !active(), or normal completion.
		struct ListenerGuard {
			GainDrive* self;
			int        gen;
			ListenerGuard(GainDrive* s, int g) : self(s), gen(g) {
				int n = ++self->cast_sse_listeners_;
				++self->cast_wd_gen_;
				if (self->debug_)
					std::cout << stamp() << "Cast: SSE listener attached, count="
					          << n << std::endl;
				}
			~ListenerGuard() {
				int n = --self->cast_sse_listeners_;
				int g = ++self->cast_wd_gen_;
				if (self->debug_)
					std::cout << stamp() << "Cast: SSE listener detached, count="
					          << n << std::endl;
				if (n != 0 || !self->cast_manager_.active()) return;
				// A listener displaced by a takeover must not arm a watchdog
				// against the session that replaced it: the new owner may not
				// have opened its own stream yet, so the listener count is
				// legitimately 0 for a moment.
				if (self->cast_session_gen_.load() != gen) return;
				GainDrive* gd = self;
				int        sg = gen;
				std::thread([gd, g, sg] {
					std::this_thread::sleep_for(std::chrono::seconds(CAST_IDLE_GRACE_S));
					if (gd->cast_wd_gen_.load() != g)        return; // newer event
					if (gd->cast_sse_listeners_.load() != 0) return; // listener back
					if (!gd->cast_manager_.active())         return; // already stopped
					if (gd->cast_session_gen_.load() != sg)  return; // another session
					std::cout << stamp() << "Cast: no SSE listener for "
					          << CAST_IDLE_GRACE_S << "s, auto-stopping"
					          << std::endl;
					gd->cast_teardown();
					}).detach();
				}
			};
		auto guard = std::make_shared<ListenerGuard>(this, session_gen);

		res.set_chunked_content_provider("text/event-stream",
			[this, guard, session_gen](size_t, httplib::DataSink& sink) -> bool {
				auto s = cast_manager_.wait_status(15000);
				if (!cast_manager_.active())                 return false;
				if (cast_session_gen_.load() != session_gen) return false;
				// The same description castLoad's reply carried, repeated on
				// every push.  Not redundancy: a LOAD that was refused is
				// retried one rung down the ladder - a film becoming its
				// soundtrack - and the reply the client read describes the
				// attempt that failed.  Without this the info panel goes on
				// claiming "as stored, MP4" over a FLAC soundtrack until the
				// page is reloaded, which is precisely the "reporting the
				// reload rather than the stream" that castSession exists to
				// prevent.
				const CastStreamInfo st = cast_stream();
				// Something to say to the person, for the failures that produce
				// no status of their own - a LOAD the receiver was never told
				// about. `noticeSeq` is what lets a client show one exactly
				// once: wait_status() republishes an unchanged status every
				// fifteen seconds, and a notice without a sequence would be
				// shown again on every one of them.
				const auto notice = cast_manager_.notice();
				std::string event = "data: " + nlohmann::json({
					{"playerState", s.player_state},
					{"currentTime", s.current_time},
					{"duration",    s.duration},
					{"idleReason",  s.idle_reason},
					{"notice",      notice.text},
					{"noticeSeq",   notice.seq},
					{"startOffset", last_cast_offset_},
					{"audioOnly",          st.audio_only},
					{"receiverShowsVideo", st.receiver_video},
					{"contentType",        st.mime},
					{"sentSuffix",         st.suffix},
					{"sentBitRate",        st.bitrate},
					{"tier",               st.tier},
					{"volume", volume_json(cast_manager_.volume_state())}}).dump()
					+ "\n\n";
				return sink.write(event.data(), event.size());
				});
		});

	// castSession - non-blocking snapshot of current cast session state.
	// Used by the browser on page load to restore the cast UI after a reload.
	server_.Get("/rest/castSession.view", [this](const httplib::Request& req,
	                                             httplib::Response& res) {
		if (!check_auth(req, res, store_)) return;
		bool use_json = (fmt_of(req) == "json");
		if (!check_cast_perm(req, res, store_, use_json)) return;
		if (!check_cast_local(req, res, use_json)) return;

		// Someone else's session is not visible here. This is what stops a
		// second browser adopting it wholesale on page load - showShell()
		// restores the cast UI from whatever this returns.
		if (!cast_manager_.active() || !cast_owned_by(req)) {
			res.set_content(subsonic_ok_json([](nlohmann::json& r) {
				r["castSession"]["active"] = false;
				}), "application/json");
			return;
			}

		auto st = cast_manager_.get_status();
		double song_duration = 0.0;
		if (!last_cast_song_id_.empty()) {
			auto song = store_.get_song(std::stoi(last_cast_song_id_));
			if (song) song_duration = song->duration;
			}
		res.set_content(subsonic_ok_json([&](nlohmann::json& r) {
			r["castSession"]["active"]       = true;
			r["castSession"]["deviceId"]     = cast_manager_.get_device_id();
			r["castSession"]["deviceName"]   = cast_manager_.get_device_name();
			// The model is what tells a WiiM from a generic receiver, and a
			// reloaded page has no device list to look the id up in.
			r["castSession"]["deviceModel"]  = cast_manager_.get_device_model();
			r["castSession"]["songId"]       = last_cast_song_id_;
			r["castSession"]["startOffset"]  = last_cast_offset_;
			r["castSession"]["playerState"]  = st.player_state;
			r["castSession"]["currentTime"]  = st.current_time;
			r["castSession"]["duration"]     = st.duration;
			r["castSession"]["songDuration"] = song_duration;
			// As on castEvents, and for the reason every other field here is
			// repeated: a page that reloads has no SSE push to have read it
			// from. Cleared by the next load, so it describes this attempt.
			const auto notice = cast_manager_.notice();
			r["castSession"]["notice"]       = notice.text;
			r["castSession"]["noticeSeq"]    = notice.seq;
			// The same description castLoad's reply carries, and it must stay
			// the same: a reloaded page has no castLoad response to have read
			// it from, and a client that drew one thing before the reload and
			// another after would be reporting the reload rather than the
			// stream.
			const CastStreamInfo st = cast_stream();
			r["castSession"]["audioOnly"]          = st.audio_only;
			r["castSession"]["receiverShowsVideo"] = st.receiver_video;
			r["castSession"]["contentType"]        = st.mime;
			r["castSession"]["sentSuffix"]         = st.suffix;
			r["castSession"]["sentBitRate"]        = st.bitrate;
			r["castSession"]["tier"]               = st.tier;
			// Which subtitle track is on, in the same 1..n numbering castLoad
			// and castControl take, so a client that has just reloaded can
			// mark its picker without asking the receiver anything.
			auto cap = cast_manager_.caption_state();
			r["castSession"]["trackId"] = cap.active_track_ids.empty()
			    ? 0 : cap.active_track_ids.front();
			r["castSession"]["volume"] = volume_json(cast_manager_.volume_state());
			}), "application/json");
		});

	// castControl - send play/pause/seek to the Chromecast.
	server_.Get("/rest/castControl.view", [this](const httplib::Request& req,
	                                             httplib::Response& res) {
		if (!check_auth(req, res, store_)) return;
		bool use_json = (fmt_of(req) == "json");
		if (!check_cast_perm(req, res, store_, use_json)) return;
		if (!check_cast_local(req, res, use_json)) return;
		// As castLoad: a non-owner controls nothing and is told the session it
		// thinks it has is not there.
		if (!cast_manager_.active() || !cast_owned_by(req)) {
			const char* msg = "Cast not active.";
			res.set_content(use_json ? subsonic_error_json(0, msg)
			                         : subsonic_error(0, msg),
			                use_json ? "application/json" : "application/xml");
			return;
			}
		std::string action;
		auto ai = req.params.find("action");
		if (ai != req.params.end()) action = ai->second;

		if (action == "pause")      cast_manager_.cast_pause();
		else if (action == "play") {
			auto st = cast_manager_.get_status();
			if (st.player_state == "PAUSED") {
				cast_manager_.cast_play();
				} else if (st.player_state == "IDLE" && !last_cast_song_id_.empty()) {
				// Session timed out during a long pause - re-issue a full load from
				// the saved position so the Chromecast can restart the stream.
				int  sid  = to_int(last_cast_song_id_, -1);
				auto song = store_.get_song(sid);
				if (song) {
					float pos = cast_manager_.last_known_time();
					// The caption the viewer had on, not none: this recovers a
					// session that timed out mid-film, and coming back without
					// the subtitles would be a second thing to fix by hand.
					auto cap = cast_manager_.caption_state();
					int  track = cap.active_track_ids.empty()
					    ? 0 : cap.active_track_ids.front();
					cast_load_song(req, *song, sid,
					               pos > 0.5f ? pos : 0.0f, track);
					}
				}
			}
		else if (action == "seek") {
			auto ti = req.params.find("time");
			if (ti != req.params.end())
				cast_manager_.cast_seek(to_float(ti->second, 0.0f));
			}
		else if (action == "captions") {
			// trackId numbers the caption tracks 1..n as getVideoInfo lists
			// them; 0 or absent turns them off.
			int track_id = to_int(req.get_param_value("trackId"), 0);
			std::vector<int> ids;
			if (track_id > 0) ids.push_back(track_id);
			if (!cast_manager_.cast_tracks(ids))
				std::cout << stamp() << "Cast: captions trackId=" << track_id
				          << " not applied - no media session yet" << std::endl;
			}
		else if (action == "volume") {
			// Absolute level, 0..1; the client does its own stepping. The
			// echo comes back on the SSE stream when the receiver reports it.
			auto li = req.params.find("level");
			if (li == req.params.end()) {
				const char* msg = "Required parameter missing: level.";
				res.set_content(use_json ? subsonic_error_json(10, msg)
				                         : subsonic_error(10, msg),
				                use_json ? "application/json" : "application/xml");
				return;
				}
			cast_manager_.cast_volume(to_float(li->second, 0.0f));
			}

		res.set_content(use_json ? subsonic_ok_json() : subsonic_ok(),
		                use_json ? "application/json" : "application/xml");
		});

	// castWiimEq: a relay to the session device's LinkPlay equalizer API.
	//
	// A WiiM is a Cast receiver and a LinkPlay device on one address, and its
	// equalizer is reachable only over the latter (android/WIIM.md). The
	// Android app speaks it directly from the phone; a browser cannot (the
	// self-signed certificate and the absent CORS headers each stop it), so
	// the server relays. The address is always the active session's device,
	// never a parameter: a castRole account must not get a relay it can point
	// at arbitrary LAN hosts.
	//
	// JSON only, like castSession: the web client is the only caller and the
	// XML arm would be dead code.
	server_.Get("/rest/castWiimEq.view", [this](const httplib::Request& req,
	                                            httplib::Response& res) {
		if (!check_auth(req, res, store_)) return;
		{
		bool use_json = (fmt_of(req) == "json");
		if (!check_cast_perm(req, res, store_, use_json)) return;
		if (!check_cast_local(req, res, use_json)) return;
		}
		auto err = [&](int code, const char* msg) {
			res.set_content(subsonic_error_json(code, msg), "application/json");
			};
		// As castControl: a non-owner controls nothing.
		if (!cast_manager_.active() || !cast_owned_by(req)) {
			err(0, "Cast not active.");
			return;
			}
		const std::string address = cast_manager_.get_device_address();
		const std::string action  = req.get_param_value("action");

		// Every mutation's reply re-reads EQGetBand and describes what the
		// device says rather than what was assumed: the Android client's
		// re-read-after-write rule, folded into the one round trip. A failed
		// re-read after a command that succeeded is still an error: the
		// client keeps its optimistic value and shows it.
		auto reply_state = [&]() {
			auto st = wiimeq::state(address);
			if (!st) { err(0, "The WiiM device did not answer."); return; }
			res.set_content(subsonic_ok_json([&](nlohmann::json& r) {
				r["wiimEq"]["on"]     = st->on;
				r["wiimEq"]["preset"] = st->preset;
				// Absent bands is the degraded mode: switch and preset list
				// only.
				if (!st->bands.empty()) r["wiimEq"]["bands"] = st->bands;
				}), "application/json");
			};

		if (action == "state") reply_state();
		else if (action == "presets") {
			auto names = wiimeq::presets(address);
			if (!names) { err(0, "The WiiM device did not answer."); return; }
			res.set_content(subsonic_ok_json([&](nlohmann::json& r) {
				r["wiimEq"]["presets"] = *names;
				}), "application/json");
			}
		else if (action == "load") {
			auto ni = req.params.find("name");
			if (ni == req.params.end()) {
				err(10, "Required parameter missing: name."); return;
				}
			if (!wiimeq::load_preset(address, ni->second)) {
				err(0, "The WiiM device did not answer."); return;
				}
			reply_state();
			}
		else if (action == "on" || action == "off") {
			if (!wiimeq::set_on(address, action == "on")) {
				err(0, "The WiiM device did not answer."); return;
				}
			reply_state();
			}
		else if (action == "setBands") {
			// A comma list of exactly ten values 0-99, refused otherwise:
			// the all-ten-or-nothing rule holds at the write too.
			std::vector<int> bands;
			const std::string bs = req.get_param_value("bands");
			bool   bad = bs.empty();
			size_t pos = 0;
			while (!bad) {
				size_t c   = bs.find(',', pos);
				int    v   = to_int(bs.substr(pos, c == std::string::npos
				                              ? std::string::npos : c - pos), -1);
				if (v < wiimeq::LEVEL_MIN || v > wiimeq::LEVEL_MAX) bad = true;
				else bands.push_back(v);
				if (c == std::string::npos) break;
				pos = c + 1;
				}
			if (bad || bands.size() != 10) {
				err(10, "Parameter bands must be ten comma-separated"
				        " values 0-99.");
				return;
				}
			if (!wiimeq::set_bands(address, bands)) {
				err(0, "The WiiM device did not answer."); return;
				}
			reply_state();
			}
		else err(0, "Unknown action.");
		});

	// castLoad - instruct the Chromecast to fetch and play a song.
	// Separate from stream.view so the browser triggers the cast load without
	// making a Range request that httplib would reject (stream.view returns 204,
	// but httplib overrides 204+Range to 416 when content_length is 0).
	server_.Get("/rest/castLoad.view", [this](const httplib::Request& req,
	                                          httplib::Response& res) {
		if (!check_auth(req, res, store_)) return;
		bool use_json = (fmt_of(req) == "json");
		if (!check_cast_perm(req, res, store_, use_json)) return;
		if (!check_cast_local(req, res, use_json)) return;

		auto err = [&](int code, const char* msg) {
			res.set_content(use_json ? subsonic_error_json(code, msg)
			                         : subsonic_error(code, msg),
			                use_json ? "application/json" : "application/xml");
			};

		// "Cast not active" is the honest answer for a non-owner too: from that
		// client's point of view it has no session.
		if (!cast_manager_.active() || !cast_owned_by(req)) {
			err(0, "Cast not active.");
			return;
			}

		auto it = req.params.find("id");
		if (it == req.params.end()) {
			err(10, "Required parameter missing: id."); return;
			}

		auto song = store_.get_song(to_int(it->second, -1));
		if (!song) { err(70, "Song not found."); return; }
		// The uploads root is personal, and this is an id-addressed read like
		// stream.view - more so: the cast token it mints is then accepted by
		// stream.view with no account and no bitrate cap at all, so skipping
		// the check here skips it everywhere downstream.
		if (!check_item_read_perm(req, res, store_, uploads_root_name_,
		                          song->path, use_json)) return;

		auto to_it = req.params.find("timeOffset");
		float cast_offset = to_it != req.params.end()
		    ? to_float(to_it->second, 0.0f) : 0.0f;
		// trackId, not captionId: the cast API numbers caption tracks 1..n in
		// the order getVideoInfo lists them, and 0 means none.  captionId
		// cannot say "none" - SIDECAR_CAPTION_INDEX is -1 and so is a missing
		// parameter, so "off" and "the sidecar file" would be one value.
		int track_id = to_int(req.get_param_value("trackId"), 0);
		// The reply says what was actually sent - the picture or only the
		// soundtrack, the contentType declared, and the container, bitrate and
		// tier the receiver will get - so the client draws what happened
		// rather than deciding it a second time from the device list and the
		// codec pair.
		CastStreamInfo sent = cast_load_song(req, *song,
		                                     to_int(it->second, -1),
		                                     cast_offset, track_id);
		res.set_content(
			use_json
			    ? subsonic_ok_json([&sent](nlohmann::json& r) {
			          r["castLoad"]["audioOnly"]   = sent.audio_only;
			          r["castLoad"]["receiverShowsVideo"] = sent.receiver_video;
			          r["castLoad"]["contentType"] = sent.mime;
			          r["castLoad"]["sentSuffix"]  = sent.suffix;
			          r["castLoad"]["sentBitRate"] = sent.bitrate;
			          r["castLoad"]["tier"]        = sent.tier;
			          })
			    : subsonic_ok([&sent](XMLDocument& doc, XMLElement* root) {
			          auto* e = doc.NewElement("castLoad");
			          e->SetAttribute("audioOnly",   sent.audio_only);
			          e->SetAttribute("receiverShowsVideo",
			                          sent.receiver_video);
			          e->SetAttribute("contentType", sent.mime.c_str());
			          e->SetAttribute("sentSuffix",  sent.suffix.c_str());
			          e->SetAttribute("sentBitRate", sent.bitrate);
			          e->SetAttribute("tier",        sent.tier.c_str());
			          root->InsertEndChild(e);
			          }),
			use_json ? "application/json" : "application/xml");
		});
	}

GainDrive::CastStreamInfo GainDrive::cast_stream() const
	{
	std::lock_guard<std::mutex> lk(cast_stream_mu_);
	return last_cast_stream_;
	}

void GainDrive::set_cast_stream(const CastStreamInfo& s)
	{
	std::lock_guard<std::mutex> lk(cast_stream_mu_);
	last_cast_stream_ = s;
	}

void GainDrive::cast_teardown()
	{
	cast_manager_.stop();
	last_cast_song_id_.clear();
	last_cast_offset_ = 0.0f;
	set_cast_stream({});
		{
		// Releasing the in-use count is what lets the transcode cache prune a
		// soundtrack nobody is listening to any more.
		std::lock_guard<std::mutex> lk(cast_warm_mu_);
		cast_warm_entry_.reset();
		}
		{
		std::lock_guard<std::mutex> lk(cast_owner_mu_);
		cast_owner_user_.clear();
		cast_owner_controller_.clear();
		}
	// After the owner is cleared, not before: the bump is the signal a
	// displaced castEvents connection watches for, and it must not see a
	// session that is half torn down.
	++cast_session_gen_;
	}

// Twelve hours, matching CastManager's CAST_TOKEN_TTL, but for a stronger
// reason than the backstop that one describes. A client holding its own Cast
// channel does **not** re-issue the LOAD to seek: the receiver seeks by byte
// range against the URL it already has, and goes on fetching that same URL for
// as long as the track is open. So this is not a timeout nobody reaches, it is
// the length of time a receiver may still come back - and shortening it would
// break a seek an hour into a concert recording.
static constexpr auto   STREAM_GRANT_TTL = std::chrono::hours(12);
// A bound, because any authenticated account can mint and nothing else evicts.
// Entries are a few dozen bytes; the number is about not growing without
// limit, not about memory.
static constexpr size_t STREAM_GRANT_MAX = 1024;

std::string GainDrive::mint_stream_grant(const std::string& user, int song_id,
                                         int cover_id)
	{
	// 128 bits, and **the return value checked**, for the reason
	// CastManager::mint_token() records: ignoring it leaves the buffer holding
	// whatever was on the stack, which is a guessable value for a credential
	// that skips authentication entirely.
	uint8_t bytes[16];
	if (RAND_bytes(bytes, sizeof(bytes)) != 1) {
		std::cout << stamp() << "RAND_bytes failed; refusing to mint a stream "
		          << "grant" << std::endl;
		return {};
		}
	char hex[33];
	for (int i = 0; i < 16; i++) snprintf(hex + 2*i, 3, "%02x", bytes[i]);

	const auto now = std::chrono::steady_clock::now();
	std::lock_guard<std::mutex> lk(grant_mu_);
	// Swept on the way in rather than on a timer, because a grant is only ever
	// reached through this map: nothing else has to notice one has expired.
	for (auto it = grants_.begin(); it != grants_.end(); )
		it = (it->second.expires <= now) ? grants_.erase(it) : std::next(it);
	// The oldest goes, not the table.
	//
	// The login throttle clears wholesale at its cap and is right to: every
	// entry there is re-creatable at no cost. A grant is not. A receiver holds
	// its URL for the length of a track and re-fetches it on every seek, with
	// nothing on this server able to tell it to ask again - so clearing would
	// stop music that is playing, on a machine busy enough to reach the cap.
	// Expiry order is mint order, the lifetime being fixed.
	if (grants_.size() >= STREAM_GRANT_MAX) {
		auto oldest = grants_.begin();
		for (auto it = grants_.begin(); it != grants_.end(); ++it)
			if (it->second.expires < oldest->second.expires) oldest = it;
		grants_.erase(oldest);
		}
	grants_[hex] = {user, song_id, cover_id, now + STREAM_GRANT_TTL};
	return hex;
	}

std::optional<GainDrive::StreamGrant>
GainDrive::grant_lookup(const std::string& token)
	{
	if (token.empty()) return std::nullopt;
	const auto now = std::chrono::steady_clock::now();
	std::optional<StreamGrant> found;
	{
	std::lock_guard<std::mutex> lk(grant_mu_);
	// Walked rather than looked up, so the comparison can be constant-time.
	// CastManager's token_eq() gives the reasoning - 128 bits makes timing
	// academic, but the wrong primitive should not be the thing deciding
	// whether an unauthenticated request is served - and the cap above is what
	// keeps the walk bounded.
	for (const auto& [tok, g] : grants_) {
		if (tok.size() != token.size()) continue;
		unsigned diff = 0;
		for (size_t i = 0; i < tok.size(); ++i)
			diff |= static_cast<unsigned char>(tok[i])
			      ^ static_cast<unsigned char>(token[i]);
		if (diff) continue;
		if (g.expires <= now) return std::nullopt;
		found = g;
		break;
		}
	}
	if (!found) return std::nullopt;
	// A grant names an account, and twelve hours is long enough for that
	// account to be disabled or deleted in the meantime. The token must not
	// outlive the access it stood in for, so the account is re-checked at
	// every use. Done here rather than in the three callers, so stream,
	// captions and cover art cannot disagree about a revoked account; and
	// outside the grant lock, because get_user takes the database mutex and
	// nothing orders the two.
	auto ui = store_.get_user(found->user);
	if (!ui || ui->disabled) return std::nullopt;
	return found;
	}

std::string GainDrive::stream_grant_user(const std::string& token, int song_id)
	{
	auto g = grant_lookup(token);
	// Scoped to one song, so presenting it for another is refused exactly as
	// if it were absent - the rule castToken already follows.
	if (!g || g->song_id != song_id) return {};
	return g->user;
	}

bool GainDrive::grant_allows_cover(const std::string& token, int cover_id)
	{
	auto g = grant_lookup(token);
	// The >= 0 matters: a song with no artwork has cover_id -1, and so does a
	// request whose id did not parse. Without it those two would agree and a
	// grant for an art-less track would authorise `getCoverArt?id=nonsense`.
	return g && g->cover_id >= 0 && g->cover_id == cover_id;
	}

bool GainDrive::grant_allows_captions(const std::string& token, int song_id)
	{
	auto g = grant_lookup(token);
	return g && g->song_id == song_id;
	}

bool GainDrive::cast_owned_by(const httplib::Request& req)
	{
	std::string controller = req.get_param_value("castController");
	if (controller.empty()) return false;
	std::lock_guard<std::mutex> lk(cast_owner_mu_);
	return !cast_owner_controller_.empty()
	    && cast_owner_controller_ == controller
	    && cast_owner_user_       == req.get_param_value("u");
	}

// Casting is offered only to a browser on the same network as the server, so
// that opening this client from a hotel in another country cannot start music
// playing in an empty house. A VPN deliberately does not qualify: a full
// tunnel from that same hotel reaches the speakers just as well, which is the
// whole reason local_nets() excludes point-to-point interfaces.
//
// The session is ended rather than merely frozen. Somebody who walks out of
// the house mid-album wants the music to stop, and without this it would stop
// anyway - but only once castEvents stopped being renewed and the
// CAST_IDLE_GRACE_S watchdog fired, which is a deterministic outcome reached
// by an indeterminate route.
bool GainDrive::check_cast_local(const httplib::Request& req,
                                 httplib::Response& res, bool use_json)
	{
	if (client_is_local(req)) return true;

	const std::string who = client_addr(req);
	// Logged on every refusal because this is the only place a proxy that
	// forgets X-Forwarded-For becomes visible: it would otherwise report every
	// caller in the world as loopback, and the check would pass in silence.
	std::cout << stamp() << "Cast refused: " << log_safe(who, 64)
	          << " is not on a network this server is attached to"
	          << std::endl;

	if (cast_owned_by(req)) {
		std::cout << stamp() << "Cast: owner left the network; ending session"
		          << std::endl;
		cast_teardown();
		}

	// Error 50 is the Subsonic code for "not authorised", which is the only
	// one that fits; the message says network rather than permission so that
	// the cause is not mistaken for a castRole that has been taken away.
	const char* msg = "Casting is only available on the server's own network.";
	res.set_content(use_json ? subsonic_error_json(50, msg)
	                         : subsonic_error(50, msg),
	                use_json ? "application/json" : "text/xml");
	return false;
	}

void GainDrive::cast_claim(const std::string& user,
                           const std::string& controller)
	{
		{
		std::lock_guard<std::mutex> lk(cast_owner_mu_);
		cast_owner_user_       = user;
		cast_owner_controller_ = controller;
		}
	++cast_session_gen_;
	}

bool GainDrive::cast_video_refused(const std::string& device_id,
                                   const std::string& codec_pair)
	{
	if (device_id.empty() || codec_pair.empty()) return false;
	std::string list = store_.get_setting(CAST_NOVIDEO_KEY + device_id);
	// Wrapped in commas both sides so "h264/aac" cannot match inside
	// "h264/aac_latm".
	return ("," + list + ",").find("," + codec_pair + ",") != std::string::npos;
	}

void GainDrive::cast_note_video_refused(const std::string& device_id,
                                        const std::string& codec_pair)
	{
	if (device_id.empty() || codec_pair.empty()) return;
	if (cast_video_refused(device_id, codec_pair)) return;
	std::string list = store_.get_setting(CAST_NOVIDEO_KEY + device_id);
	if (!list.empty()) list += ",";
	list += codec_pair;
	store_.set_setting(CAST_NOVIDEO_KEY + device_id, list);
	std::cout << stamp() << "Cast: device " << device_id << " refused "
	          << codec_pair << "; sending the soundtrack for it from now on"
	          << std::endl;
	}

CastManager::LoadRequest
GainDrive::soundtrack_load(const std::string& base,
                           const MediaStore::SongInfo& song, int song_id,
                           float offset, bool is_fallback,
                           CastStreamInfo& desc)
	{
	CastManager::LoadRequest lr;
	desc = CastStreamInfo{};

	// The same answer cast_load_song() reached when it decided this was worth
	// doing at all - one definition, in codecs.hh, for the reason stated there.
	const std::string fmt = cast_soundtrack_format(song.audio_codec);
	if (fmt.empty()) return lr;

	// No captions collected, which is not an omission: there is no picture to
	// put them on, and an empty list is what leaves the minted token unable to
	// fetch one at all.
	const std::string token = cast_manager_.mint_token(song_id, {});
	if (token.empty()) return lr;

	const std::string sid_s = std::to_string(song_id);
	// Naming an audio format for a video *is* the request for its soundtrack -
	// audio_only_request() in codecs.hh - so this one parameter is the whole of
	// it on the server side.
	lr.url  = base + "stream.view?id=" + sid_s + "&castToken=" + token
	        + "&format=" + fmt;
	lr.mime = std::string(codec_to_mime(fmt));
	// Native seek: the URL serves the whole file and the LOAD says where to
	// begin, so the receiver's clock is absolute.
	lr.current_time = offset;
	lr.duration     = song.duration;

	desc.audio_only     = true;
	// No picture reaches the receiver on this route, by construction.  A client
	// keeps its own copy of the picture on this, never on audio_only.
	desc.receiver_video = false;
	desc.mime           = lr.mime;
	desc.suffix         = fmt;

	Streamer::SongInfo si = streamer_song(song, store_.abs_path(song.path));
	// Resolved here rather than inside the lambda so the figures reported to
	// the client are the ones the warm actually produces.  It is pure
	// negotiation against the source - no I/O - so hoisting it costs nothing
	// and two copies of it could disagree.
	auto plan = Streamer::plan_transcode(si, fmt, 0, 0);
	// A copy plan is exactly the one whose bitrate is 0.
	const bool copying = plan.bitrate == 0;
	// The same two words the video ladder uses, and they mean the same thing
	// here: "remux" is -c:a copy of the track the file already holds, "encode"
	// is a decode and a lossless re-encode.  It is the answer to "why does this
	// sound worse on the television", which is the whole reason these fields
	// are reported rather than re-derived by the client.
	desc.tier = copying ? "remux" : "encode";
	// 0 for anything that is not a fixed-rate encode, which is two cases: a
	// copy, and a *lossless* encode, where plan.bitrate is only the non-zero
	// marker meaning "encode" and describes nothing about the bytes.
	desc.bitrate = (plan.target && plan.target->lossy) ? plan.bitrate : 0;

	// Captured now: by the time prepare runs, the session's device is still the
	// same one, but reading it on that thread would be a second answer to a
	// question already settled here.
	const std::string dev  = cast_manager_.get_device_id();
	const std::string pair = song.video_codec + "/" + song.audio_codec;

	// The soundtrack of a film is a transcode of a two-hour AC3 track, and
	// serve() answers it out of the transcode cache - which materialises the
	// whole file before the first byte.  A receiver drops a session after about
	// a minute with no data on the HTTP body, so that wait has to happen before
	// it is told anything at all.  This runs on CastManager's load worker,
	// which already treats a newer load_gen_ as a cancellation.
	//
	// The entry is kept alive afterwards: it is an RAII in-use count and
	// prune() skips in-use keys, so releasing it here would let the file be
	// evicted between the warm and the receiver's first GET.
	lr.prepare = [this, si, sid_s, plan, copying, is_fallback, desc, dev,
	              pair]() {
		// Before the warm, not after: a client asking castSession during the
		// minute ffmpeg takes should already be told what it is waiting for.
		// This hook running at all is the proof that the first attempt was
		// refused - CastManager only reaches a fallback down that path - which
		// is why both of these live here and nowhere else.
		if (is_fallback) {
			cast_note_video_refused(dev, pair);
			set_cast_stream(desc);
			}
		auto entry = Streamer::cache_entry(si, transcode_cache_, plan);
		if (!entry) {
			// Logged because a cast that dies about a minute in is this line.
			// Whether it is fatal depends on which plan failed, and the
			// asymmetry is the point: an *encode* that could not be cached may
			// still play as a pipe, so the LOAD goes out.  A *copy* that could
			// not be cached means the argv itself failed, and serve() would run
			// the identical argv down the piped path and fail identically - so
			// pointing the receiver at that URL buys a dead session with
			// nothing to explain it.
			std::cout << stamp()
			          << "Cast: no cache entry for soundtrack of song="
			          << sid_s
			          << (copying ? ", copy failed; not loading"
			                      : ", streaming unwarmed") << std::endl;
			return !copying;
			}
		std::cout << stamp() << "Cast: soundtrack warmed song=" << sid_s
		          << " " << (entry->hit() ? "hit" : "built")
		          << " bytes=" << entry->size() << std::endl;
		std::lock_guard<std::mutex> lk(cast_warm_mu_);
		cast_warm_entry_ = std::move(entry);
		return true;
		};
	return lr;
	}

GainDrive::CastStreamInfo
GainDrive::cast_load_song(const httplib::Request& req,
                          const MediaStore::SongInfo& song,
                          int song_id, float offset, int track_id)
	{
	// Everything the receiver fetches hangs off this, and every one of them
	// carries the cast token rather than the account's credentials: a
	// television is not a place to leave a password, and the token is already
	// what stream.view accepts.
	//
	// The configured public_url wins over the request's own Host header, and
	// on a public deployment it must: Host and X-Forwarded-Proto are client
	// input, so believing them let any cast-role account point the receiver
	// at an origin of its choosing - which hands that origin the cast token
	// in the query string, and the token is accepted by stream.view with no
	// account attached. On a LAN with no public_url configured the header is
	// the only source there is, and the peers who could abuse it are the
	// household.
	std::string base;
	if (!public_url().empty())
		base = public_url() + "/rest/";
	else {
		std::string host  = req.get_header_value("Host");
		if (host.empty()) host = "localhost";
		std::string proto = req.get_header_value("X-Forwarded-Proto");
		if (proto.empty()) proto = "http";
		base = proto + "://" + host + "/rest/";
		}
	const std::string sid_s = std::to_string(song_id);

	// A receiver that cannot display a picture is sent the film's soundtrack
	// rather than a video container it will drop the picture out of.  The
	// decision is made here, in the one place that has both the song and the
	// device, and reported to the client rather than re-derived there - the
	// same rule the three callers of the tier predicate follow.
	//
	// `video_out()` reports true for a device that announced no capabilities,
	// which is every configured one: refusing the picture on a guess is worse
	// than the guess.
	//
	// The third term is not belt and braces.  audio_only_request() is what
	// serve() will actually apply, so this has to reach the same answer or the
	// LOAD announces audio/mpeg while the video ladder serves MP4, and a
	// receiver refuses media whose type does not match what arrives.  A
	// *silent* video is the case that separates the two: it has nothing to
	// extract, so audio_only_request() keeps it on the video ladder.
	//
	// What "cannot display a picture" means is not quite the `ca` record on its
	// own any more.  A per-device preference, held server-side under the cast
	// device's id so every client agrees about a device, can override it in
	// either direction, and the two directions are deliberately not
	// symmetrical:
	//
	//  * `send` only ever *raises* a device that announced no screen, and only
	//    for a file the Direct tier sends untouched.  A WiiM plays a video
	//    file's sound perfectly well whatever the Cast documentation says, and
	//    the raw file byte-ranged off disk costs nothing at all - no ffmpeg,
	//    no wait before the LOAD.  It stops at the Direct tier because the
	//    other two are worse than extracting the sound: a remux reads and
	//    writes the whole film through the cache, and a re-encode re-encodes a
	//    picture nobody can see.  Never applied to a device that *does*
	//    announce a screen, or it would demote a remuxable film to audio.
	//  * `sound` closes the other half of the gap.  A configured device has
	//    capabilities == -1 and so reads as capable, which means a screenless
	//    device named in the config could not be told it has no screen.
	const std::string video_pref =
		store_.get_setting("cast_video:" + cast_manager_.get_device_id());
	bool shows_video = cast_manager_.device_video_out();
	const std::string codec_pair = song.video_codec + "/" + song.audio_codec;
	if (video_pref == "sound")
		shows_video = false;
	else if (video_pref == "send" && !shows_video)
		// The tier says the file can go out untouched; the second test says
		// this device has not already proved it cannot decode it.  The Direct
		// tier is built from *browser* predicates, and a Cast receiver's codec
		// support is narrower - an AV1/Opus MP4 passes it and no amplifier
		// plays it - so the tier alone is a hopeful answer rather than a
		// reliable one.  Being refused once is what makes it reliable.
		shows_video = cast_tier_for(song.codec, song.video_codec,
		                            song.audio_codec) == CastTier::Direct
		           && !cast_video_refused(cast_manager_.get_device_id(),
		                                  codec_pair);

	// Empty when there is nothing to extract - a silent film, or one the
	// scanner could not probe.  Such a video stays on the video ladder, which
	// is what keeps this predicate agreeing with the one Streamer::serve()
	// applies: a LOAD announcing audio/flac while MP4 goes out is media a
	// receiver refuses on the type alone.
	const std::string cast_fmt = cast_soundtrack_format(song.audio_codec);

	const bool audio_only = song.is_video && !shows_video && !cast_fmt.empty();

	// Two shapes, and they share only the description they produce.  The
	// soundtrack is built by soundtrack_load(), which the fallback below builds
	// again - that is why it is a function rather than a branch.
	CastManager::LoadRequest lr;
	CastStreamInfo stream;

	if (audio_only) {
		lr = soundtrack_load(base, song, song_id, offset, false, stream);
		if (lr.url.empty()) {
			std::cout << stamp() << "Cast: no soundtrack load for song=" << sid_s
			          << "; refusing to load" << std::endl;
			// Nothing was loaded, so nothing is being sent: an empty description.
			// The client hides a row it has no value for, which is the right
			// showing for a load that did not happen.
			return {};
			}
		}
	else {
		// The caption list is resolved before the token is minted, because the
		// token is scoped to this song *and* to the caption ids this LOAD is
		// about to declare - getCaptions will accept it for those and nothing
		// else.
		MediaStore::VideoStreams streams;
		std::vector<int> caption_ids;
		if (song.is_video) {
			streams = store_.get_video_streams(song_id);
			for (const auto& c : streams.captions)
				caption_ids.push_back(c.index);
			}

		// Minted per LOAD rather than per session. Everything the receiver
		// fetches carries this rather than the account's credentials - a
		// television is not a place to leave a password - so what it is worth is
		// what a leak costs: one song, for as long as this LOAD is current.
		const std::string token = cast_manager_.mint_token(song_id, caption_ids);
		if (token.empty()) {
			std::cout << stamp() << "Cast: no stream token; refusing to load"
			          << std::endl;
			return {};
			}
		const std::string tok = "&castToken=" + token;

		lr.url  = base + "stream.view?id=" + sid_s + tok;
		lr.mime = std::string(cast_mime_for(song.codec, song.video_codec,
		                                    song.audio_codec));
		// Native seek: the URL serves the whole file and the LOAD says where to
		// begin, so the receiver's clock is absolute.  That is also why the
		// caption cues need no shifting here, unlike the browser's own transcoded
		// seek - see videoShiftCues() in web/app.js for the case where they do.
		lr.current_time = offset;
		lr.duration     = song.duration;

		stream.mime = lr.mime;
		// Whether the receiver will actually *show* the film, which is a
		// different question from whether it was sent one.  Under `send` a
		// screenless device gets the whole video and displays none of it, so
		// !audio_only would tell a client to drop its own picture exactly where
		// it is most wanted - the bug that made this a separate field.
		// device_video_out() is the raw announcement here on purpose: the
		// preference decides what to send, and no preference gives a receiver a
		// screen.
		stream.receiver_video = song.is_video
		                     && cast_manager_.device_video_out();
		CastTier t  = cast_tier_for(song.codec, song.video_codec,
		                            song.audio_codec);
		stream.tier = std::string(cast_tier_name(t));
		// A remux and a re-encode both arrive as MP4; only the direct tier sends
		// the file as it stands, so only there is the file's own bitrate the one
		// going over the wire.  That is the fact a client cannot get from
		// anywhere else: the transcoded* fields describe the account ceiling,
		// which stream.view exempts for a cast token, so they describe a
		// conversion that is not happening.
		//
		// 0 on the other two tiers rather than the source's figure.  A -c copy is
		// close enough to it that quoting it would be nearly right, and nearly
		// right is the worst thing a diagnostic can be; a re-encode is not
		// fixed-rate at all.
		stream.suffix  = t == CastTier::Direct ? song.codec : "mp4";
		stream.bitrate = t == CastTier::Direct ? song.bitrate : 0;

		if (song.is_video) {
			int  n = 0;
			for (const auto& c : streams.captions) {
				++n;
				lr.caption_ids.push_back(c.index);
				lr.tracks.push_back({
					{"trackId",          n},
					{"type",             "TEXT"},
					{"subtype",          "SUBTITLES"},
					{"trackContentId",   base + "getCaptions.view?id=" + sid_s
					                     + "&captionId=" + std::to_string(c.index)
					                     + tok},
					{"trackContentType", "text/vtt"},
					// Required for a subtitle track, and one without it can be
					// dropped by the receiver with no diagnostic anywhere.  A
					// sidecar file has no language to report, so it gets the
					// ISO 639-2 code that means exactly that.
					{"language",         c.language.empty() ? "und" : c.language},
					{"name",             c.title.empty() ? "Subtitles" : c.title}
					});
				}
			if (track_id > 0 && track_id <= n)
				lr.active_track_ids.push_back(track_id);
			}

		// What to do when the receiver refuses this outright.
		//
		// Only for the one case where the answer is unambiguous: the `send`
		// preference has put a picture on a device that announced no screen, so
		// the picture was never going to be seen there and the browser is very
		// likely holding its own copy already.  Dropping to the soundtrack costs
		// nothing anyone was looking at.
		//
		// Deliberately not offered for a device that *did* announce a screen.
		// There the soundtrack is not obviously wanted, no client is holding the
		// picture to fall back to, and the panel would go on saying "Playing on
		// your TV" over sound alone.  That case is a known gap: an AV1 film cast
		// to a receiver that cannot decode it still fails, and the tier ladder
		// has nothing better to offer it - a remux is -c copy, so it would send
		// the identical codecs a second time.
		if (song.is_video && !cast_manager_.device_video_out()) {
			CastStreamInfo fb;
			auto fb_req = soundtrack_load(base, song, song_id, offset, true, fb);
			if (!fb_req.url.empty())
				lr.fallback = std::make_shared<CastManager::LoadRequest>(
				                  std::move(fb_req));
			}
		}

	// One line rather than reading it out of the LOAD dump below, which for a
	// film with several tracks is long enough to scroll past. Says what a
	// "subtitles do not appear", "no picture" or "why is this being
	// re-encoded" report needs first: whether the server thinks this is a
	// video at all, whether it decided to send only the sound, which tier the
	// fetch will land on and what that means the receiver gets, how many
	// tracks it offered, and which one it asked for.  It is also the line to
	// compare a client's info panel against, since the panel is drawn from
	// exactly these values rather than from a second derivation.
	std::cout << stamp() << "Cast: load song=" << sid_s
	          << " video=" << (song.is_video ? "yes" : "no")
	          << " audio_only=" << (audio_only ? "yes" : "no")
	          << (video_pref.empty() ? "" : " pref=" + video_pref)
	          << " screen=" << (stream.receiver_video ? "yes" : "no")
	          << (lr.fallback ? " fallback=soundtrack" : "")
	          << " mime=" << lr.mime
	          << " tier=" << stream.tier
	          << " sent=" << stream.suffix
	          << (stream.bitrate > 0 ? "@" + std::to_string(stream.bitrate) : "")
	          << " tracks=" << lr.tracks.size()
	          << " active=" << (lr.active_track_ids.empty()
	                            ? 0 : lr.active_track_ids.front())
	          << std::endl;

	last_cast_song_id_ = sid_s;
	last_cast_offset_  = 0.0f;
	set_cast_stream(stream);
	cast_manager_.load(lr);
	return stream;
	}

// A configured cast device is never confirmed by anything: mDNS does not
// announce it, and CastManager::start() neither connects nor fails, so a typo
// in --cast-device or the config file stays completely silent until someone
// tries to cast and gets a session that never begins. One probe each at startup
// turns that into a line in the log.
//
// Detached, because a device that is merely switched off costs the connect
// timeout and nothing should wait for that.
void GainDrive::probe_cast_devices_background()
	{
	auto devices = cast_manager_.cached_devices();
	std::vector<CastManager::CastDevice> manual;
	for (auto& d : devices)
		if (d.manual) manual.push_back(d);
	if (manual.empty()) return;

	std::thread([this, manual]{
		try {
			for (const auto& d : manual) {
				auto result = cast_manager_.probe(d);
				std::cout << stamp() << "Cast: configured device '" << d.name
				          << "' (" << d.address << ":" << d.port << ") "
				          << CastManager::probe_text(result) << std::endl;
				}
			}
		// An exception escaping a detached thread is std::terminate, and a
		// probe is not worth a dead server.
		catch (const std::exception& e) {
			std::cout << stamp() << "Cast device probe failed: " << e.what()
			          << std::endl;
			}
		catch (...) {
			std::cout << stamp() << "Cast device probe failed: unknown exception"
			          << std::endl;
			}
		}).detach();
	}
