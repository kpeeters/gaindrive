#pragma once

#include <string>
#include <vector>
#include <mutex>
#include <chrono>
#include <condition_variable>
#include <atomic>
#include <functional>
#include <memory>

#include <nlohmann/json.hpp>

// The TLS connection to a receiver, defined in castmanager.cc: it is an
// OpenSSL socket and nothing else, and keeping it out of this header is what
// keeps <openssl/ssl.h> out of everything that casts.
struct Tls;

class CastManager {
	public:
		struct CastDevice {
			std::string id;
			std::string name;
			// The `md` TXT record - "WiiM Amp Ultra", "Chromecast Ultra". The
			// only thing on the wire that tells one kind of receiver from
			// another, and what the picker draws ahead of the address. Empty
			// for a configured device: there is no announcement to read.
			std::string model;
			std::string address;
			int         port = 8009;
			// True for a device named in the configuration rather than found by
			// mDNS.  Nothing in the protocol reads it; it exists so the API can
			// say where an entry came from.
			bool        manual = false;
			// The `ca` TXT record, a bitmask of what the receiver can do.
			// -1 means "not announced", which is every configured device and
			// any receiver that omits the record - treated as capable, since
			// refusing the picture on a guess is worse than the guess.
			int         capabilities = -1;

			// Bit 0 of `ca` is video_out. A WiiM amp clears it; a Chromecast
			// or a television sets it. This is the whole of what distinguishes
			// a receiver that can show a film from one that can only play its
			// soundtrack - nothing in a LOAD's reply says so, and a receiver
			// that cannot display simply drops the picture.
			bool video_out() const
				{
				return capabilities < 0 || (capabilities & CA_VIDEO_OUT) != 0;
				}
			};

		static constexpr int CA_VIDEO_OUT = 0x01;

		// The id given to a configured device, derived from its address rather
		// than random.  The web client stores the id of the device it is casting
		// to and restores it after a page reload, so an id that changed across a
		// restart would break that restore.
		static std::string manual_id(const std::string& address, int port);

		struct CastStatus {
			std::string player_state = "IDLE";  // IDLE | PLAYING | PAUSED | BUFFERING
			float       current_time = 0;
			float       duration     = 0;
			int         media_session_id = 0;
			std::string idle_reason;            // FINISHED | INTERRUPTED | ERROR (when IDLE)
			};

		// Knobs on the mDNS discovery pass.  Every default reproduces the
		// behaviour discover() had before this struct existed, so the server
		// is unaffected until one of them is deliberately changed; they are
		// here so gaindrive-cast can vary each one from the command line
		// while working out why we see fewer devices than the Cast SDK does.
		struct DiscoverOpts {
			int  timeout_ms    = 4000;  // total wall clock for the pass
			int  queries       = 1;     // PTR queries sent per socket
			int  query_gap_ms  = 1000;  // gap before the 2nd; doubles after
			bool ipv4          = true;
			bool ipv6          = true;
			bool per_interface = false; // one socket per local address
			std::string iface;          // restrict to this interface ("" = all)
			bool require_id    = true;  // drop a device with no TXT id
			// Assemble devices only from records belonging to
			// _googlecast._tcp.  Off is how we find out whether a device is
			// announcing itself under some other service and being ignored.
			bool cast_service_only = true;
			bool verbose       = true;  // log every record as it arrives
			};

		// Scan for Chromecast devices via mDNS.
		std::vector<CastDevice> discover(const DiscoverOpts& opts);

		// Scan for timeout_ms milliseconds with everything else defaulted.
		std::vector<CastDevice> discover(int timeout_ms);

		// Run discover() in a background thread and cache the results.
		// Returns immediately; call cached_devices() to read the result.
		void discover_background(int timeout_ms = 4000);

		// Devices named in the configuration, for the ones mDNS cannot find.
		// Held separately from the discovery cache and merged only on the way
		// out (see cached_devices), because discover_background() replaces that
		// cache wholesale and would drop anything written into it.
		void set_manual_devices(std::vector<CastDevice> devices);

		// The discovery cache merged with the configured devices.
		//
		// Every caller goes through here - listCastDevices and startCast's id
		// lookup both scan it - so the merge happening in one place is what lets
		// the endpoints stay as they are.  A configured device whose address
		// discovery also found is dropped in discovery's favour: that entry
		// carries the friendly name from the device's own TXT record and its
		// real Cast id, where a configured one has only what someone typed.
		std::vector<CastDevice> cached_devices() const;

		// What asking a device whether it is there produced.
		//
		// SILENT is worth telling apart from UNREACHABLE: it means something
		// accepted a TLS connection on the cast port and then said nothing,
		// which is a live host that is not a Chromecast rather than a wrong
		// address.
		enum class Probe { ANSWERED, SILENT, UNREACHABLE };

		// Connect, ask for a receiver status, disconnect.  Touches none of the
		// session state - deliberately not start(), which sets active_ and
		// detaches poll_loop(), so probing a typed-in address would begin
		// casting to it.
		//
		// This is the only way a bad address is ever reported.  start() returns
		// true unconditionally and never connects, so a wrong one yields a
		// session that says it is active and never produces a status.
		Probe probe(const CastDevice& device, int timeout_ms = 6000);

		// One word for a Probe, for logging and for the command-line modes.
		static const char* probe_text(Probe result);

		// Enter cast mode: store device and generate a single-use stream token.
		bool start(const CastDevice& device);

		// Everything one LOAD message says.
		//
		// A struct rather than a parameter list because this value is needed in
		// four places at once - the call, the worker thread, the retry copy and
		// the JSON builder - so every field added as a parameter had to be
		// added four times, and a field forgotten in the retry copy is silently
		// dropped only when a LOAD fails and is re-sent. One assignment now
		// carries all of it.
		struct LoadRequest
			{
			std::string    url;
			std::string    mime;
			float          current_time = 0.0f;
			double         duration     = 0.0;
			// Side-loaded subtitle tracks, as the Cast media object spells
			// them. **Declared in every LOAD, whether or not one is active**:
			// EDIT_TRACKS_INFO can turn on a trackId the LOAD declared but
			// cannot introduce one, so tracks omitted here are tracks the
			// viewer can never reach without reloading the film.
			nlohmann::json tracks = nlohmann::json::array();
			std::vector<int> active_track_ids;
			// trackId (1-based) → the captionId getCaptions expects, so a
			// client can speak in track numbers without the server having to
			// re-probe the file to interpret one.
			std::vector<int> caption_ids;
			// Run on the worker thread before the LOAD is sent; false aborts
			// the load.  It exists so a caller can make `url` answerable
			// *before* the receiver is told to fetch it - materialising a
			// transcode-cache entry, which takes minutes for a film's
			// soundtrack and would otherwise happen while a receiver that
			// gives up after ~60 s of silence is waiting on the socket.
			// CastManager deliberately does not know what is being prepared.
			std::function<bool()> prepare;
			// What to try instead when this LOAD is refused outright, one rung
			// further down the ladder degrade_load() walks - see there.  A
			// shared_ptr because the type cannot contain itself, and because
			// both failure sites copy the whole request out under
			// status_mutex_ before acting on it.
			//
			// CastManager knows no more about what the alternative *is* than it
			// knows what `prepare` prepares.  A caller builds both; today the
			// only one is a film's soundtrack, attached when the picture was
			// sent to a receiver that announced no screen.
			std::shared_ptr<LoadRequest> fallback;
			};

		// Connect to the Chromecast, send LOAD, capture initial MEDIA_STATUS, then close.
		// The Chromecast fetches and plays independently; poll_loop maintains a separate
		// persistent connection that receives pushed status updates.
		// current_time tells the device where to start (seconds into the track).
		void load(const LoadRequest& req);

		// Stop Chromecast playback and exit cast mode.
		void stop();

		// Playback controls - each opens a fresh connection.
		void cast_pause();
		void cast_play();
		void cast_seek(float seconds);

		// Turn subtitle tracks on or off without reloading, by trackId; an
		// empty list means none. Returns false when the receiver has no media
		// session to edit yet, which is not an error - see the definition.
		bool cast_tracks(const std::vector<int>& track_ids);

		// The caption mapping the last LOAD went out with, and which of them is
		// active. Read under status_mutex_ so a caller cannot see a list from
		// one film beside the selection from another.
		struct CaptionState
			{
			std::vector<int> caption_ids;       // trackId−1 → captionId
			std::vector<int> active_track_ids;
			};
		CaptionState caption_state() const;

		// Return the last cached status.
		CastStatus get_status() const;

		// Something a person should be told about the cast session, and a
		// sequence number that changes when it does.
		//
		// It exists for the failures that produce no status of their own: a
		// LOAD abandoned because the television never finished starting up is
		// invisible to the receiver, so nothing would otherwise reach the
		// client and it would sit on "Preparing…" for ever.
		//
		// **Not a field of CastStatus**, although that is where a reader would
		// look for it: update_status() assigns status_ wholesale from each
		// push, so a notice living there would be wiped by the very status
		// published to announce it. The sequence number is what lets a client
		// show one exactly once - wait_status() republishes an unchanged status
		// every fifteen seconds.
		struct Notice
			{
			std::string text;
			int         seq = 0;
			};
		Notice notice() const;

		// The receiver's own volume, as its RECEIVER_STATUS pushes report it.
		// `known` starts false and stays false until a push carries a volume
		// block, so a client can tell "not reported yet" from level zero and
		// keep its buttons disabled rather than guess. `fixed` is the
		// receiver's controlType saying nothing can move its volume: a
		// television driving an amplifier, typically.
		struct VolumeState
			{
			bool  known = false;
			float level = 0.0f;   // 0..1
			bool  muted = false;
			bool  fixed = false;
			};
		VolumeState volume_state() const;

		// Set the receiver's volume, 0..1.
		void cast_volume(float level);

		// Block until the next status push from the Chromecast (or timeout_ms elapses).
		// Used by the SSE endpoint to stream updates to the browser.
		CastStatus wait_status(int timeout_ms = 15000);

		bool        active()          const { return active_; }
		std::string get_device_id()   const { return device_.id; }
		std::string get_device_name() const { return device_.name; }
		// The mDNS model string, empty for a configured device. A client uses
		// it to tell one kind of receiver from another after a page reload,
		// when it has no device list to look the id up in.
		std::string get_device_model()   const { return device_.model; }
		std::string get_device_address() const { return device_.address; }
		// What the session's device can do, for callers deciding *what* to
		// send it rather than how.  cast_load_song() is the one that matters:
		// a receiver with no video_out gets a film's soundtrack.
		bool        device_video_out() const { return device_.video_out(); }
		float       last_known_time() const;

		// ---- The stream token -----------------------------------------
		//
		// A Chromecast fetches stream.view and getCaptions.view for itself and
		// has no credentials to do it with, so this token stands in for them.
		// It is therefore a bearer credential that skips check_auth entirely,
		// and what it is *worth* is decided here.
		//
		// It used to be one string per cast session, minted in start() and
		// bound to nothing: while any session was live, that one value read
		// the whole library - every root and every user's private uploads -
		// with no account, no bitrate cap and no expiry. It travels in cleartext
		// to a television and appears in the LOAD message, so "it never leaves
		// the LAN" was the only thing limiting it.
		//
		// Now it is minted per LOAD and scoped to what that LOAD declared: one
		// song, and the caption ids offered for it. A leaked token buys the
		// track it was minted for, until it expires.

		// New token for one song and its caption ids. Returns the token.
		std::string mint_token(int song_id, const std::vector<int>& caption_ids);

		// The current token, or empty. Only cast_load_song() needs this, to put
		// it into the URLs the LOAD hands the receiver.
		std::string token() const;

		// Both take the id the request is asking for, and both are false if it
		// is not the one this token was minted for. `caption_id` is a
		// getCaptions captionId, including SIDECAR_CAPTION_INDEX.
		bool valid_token(const std::string& t, int song_id) const;
		bool valid_caption_token(const std::string& t, int song_id,
		                         int caption_id) const;

		// Incremented at the start of every load() call so content-provider
		// threads can detect that a new stream has started and exit promptly.
		int load_generation() const { return load_gen_.load(); }

	private:
		bool        active_ = false;
		CastDevice  device_;

		// The token and everything that bounds it, under one mutex. Kept apart
		// from status_mutex_ deliberately: this is read from httplib threads on
		// the hot path of every range request the receiver makes, and taking
		// the status lock there would queue those behind the poll loop.
		mutable std::mutex token_mutex_;
		std::string        token_;
		int                token_song_id_ = -1;
		std::vector<int>   token_caption_ids_;
		std::chrono::steady_clock::time_point token_expires_{};

		std::atomic<int> load_gen_{0};

		mutable std::mutex         tid_mutex_;
		std::string                transport_id_;    // set in load(), needed for media commands

		mutable std::mutex         status_mutex_;
		std::condition_variable    status_cv_;       // notified on every status update
		CastStatus                 status_;
		float                      last_known_time_ = 0.0f; // current_time from last non-IDLE status
		// Under status_mutex_ and published through status_cv_ like status_:
		// the SSE frame carries both, and a volume push must wake the same
		// listeners a media push does.
		VolumeState                volume_;

		// Auto-retry state.  The Default Media Receiver sometimes fails the
		// first LOAD that interrupts a currently-PLAYING media session: the
		// new media session goes straight from IDLE/INTERRUPTED to IDLE/ERROR
		// without ever reaching PLAYING.  Re-sending the same LOAD into the
		// now-quiet receiver works, which mirrors the user's manual fix of
		// clicking the same track twice.  retry_pending_ is set in load() and
		// either cleared on a non-failure state transition or consumed by
		// firing a single retry from update_status().  All three fields are
		// guarded by status_mutex_.
		bool        retry_pending_      = false;
		int         last_load_old_msid_ = 0;   // msid active when load() was called
		LoadRequest last_load_;

		// The last thing worth telling a person, under status_mutex_ with the
		// status it accompanies. See notice() above for why it does not live in
		// CastStatus.
		std::string notice_;
		int         notice_seq_ = 0;

		// Bumped by every LOAD that leaves this process, and by nothing else.
		// It is what scopes await_load_ack() to its own attempt, so the watcher
		// armed by a first LOAD stays out of the way of a degrade retry that
		// has since taken over the session.
		std::atomic<int> load_attempt_{0};

		// Every message we send carries one, and it must be fresh: a receiver
		// correlates its replies by requestId, so a re-send repeating one it
		// has already seen is worse than sending nothing. Every LOAD used to be
		// requestId 2, which was harmless only because nothing was ever sent
		// twice. Starts clear of every fixed id still in use - the playback
		// commands' 10-13 and poll_loop's 100-102.
		std::atomic<int> request_id_{1000};

		mutable std::mutex         cache_mutex_;
		std::vector<CastDevice>    devices_cache_;  // last result of discover_background()

		// Its own mutex rather than sharing cache_mutex_: a discovery pass holds
		// that one to publish, and the configured list must stay readable
		// throughout.
		mutable std::mutex         manual_mutex_;
		std::vector<CastDevice>    manual_devices_;

		std::atomic<bool>          poll_active_{false};

		// Parse a MEDIA_STATUS message and store the result in status_.
		void update_status(const nlohmann::json& msg);

		// Read the volume block out of a RECEIVER_STATUS, if it carries one.
		void update_volume(const nlohmann::json& msg);

		// A LOAD the receiver refused, reported on the media namespace as an
		// ERROR/LOAD_FAILED rather than as a status.  Publishes the IDLE/ERROR
		// the receiver did not send and consumes the retry.
		void note_load_failure(int media_session_id);

		// One rung down after a refused LOAD, rewriting `req` in place.  The
		// rungs, in order, and each is "degrade rather than repeat" - replaying
		// a LOAD the receiver has already rejected only fails again:
		//
		//   1. drop the subtitle tracks.  One unreachable track URL fails the
		//      whole LOAD, and this costs the captions and nothing else.
		//   2. become `fallback`, which is how a film the receiver cannot
		//      decode becomes its soundtrack.
		//
		// Returns the reason, for the log, or an empty string when nothing is
		// left to try.  Static and free of member state so both failure sites
		// share one definition of the ladder; a second copy of it is exactly
		// how the two would come to disagree about what a failure means.
		static const char* degrade_load(LoadRequest& req);

		// Re-send a LOAD on a detached thread, abandoning it if the user has
		// asked for something else in the meantime.
		// `why` is what the receiver did; `how` is the rung degrade_load()
		// chose. Both are logged, because a retry that keeps happening and a
		// retry that keeps changing shape are different faults.
		void spawn_retry(const LoadRequest& req, const char* why,
		                 const char* how);

		// Open a fresh connection and send one media-namespace command.
		void send_media_cmd(const nlohmann::json& payload);

		// Background thread: maintains a persistent TLS connection to the Chromecast,
		// responds to PING heartbeats, and processes pushed MEDIA_STATUS messages.
		void poll_loop();

		// Worker spawned by load() - does the actual TLS connect + LOAD.
		// Checks load_gen_ against gen at each blocking step and aborts early if
		// a newer load() has been called.
		void load_worker(LoadRequest req, int gen);

		// The transport of a running Default Media Receiver on `t`, launching
		// one if there is none. The counterpart of the Android client's
		// ensureTransport, and the deadlines it waits under are the ones at the
		// top of castmanager.cc.
		std::string ensure_transport(Tls& t, const std::string& src, int gen);

		// One LOAD, from opening the socket to writing the message. Resolves a
		// transport if none is cached, and reports every way of giving up
		// through note_load_abandoned(). `used_cached_transport` says whether
		// the LOAD went to a transport we already had, which is what decides
		// whether a re-send should throw that transport away first.
		// Returns the attempt number to hand await_load_ack(), or 0 if nothing
		// was sent.
		int send_load_once(const LoadRequest& req, int gen,
		                   bool* used_cached_transport);

		// Wait LOAD_ACK_WAIT_MS; true if the LOAD was acknowledged, or is no
		// longer ours to worry about.
		//
		// **retry_pending_ is the acknowledgement**, and that is where this
		// differs from the Android client, which counts MEDIA_STATUS arrivals.
		// It cannot here: poll_loop() sends its own GET_STATUS every second and
		// a receiver that dropped the LOAD still answers those, so a bare
		// arrival proves nothing. retry_pending_ is armed by load() and cleared
		// by update_status() only for a status carrying the *new*
		// mediaSessionId, which is exactly the question being asked - with the
		// stale-push filtering already written and already commented there.
		bool await_load_ack(int gen, int attempt);

		// A LOAD that never reached the receiver: publishes the IDLE/ERROR
		// nothing else will, records the notice, and consumes the retry flag
		// *without* degrading. See the definition for why the ladder has no
		// rung for this.
		void note_load_abandoned(const char* what);

		// The next requestId. See request_id_.
		int next_request_id();

		// The LOAD message itself.  One builder, which is why send_load_once()
		// is one function rather than a fast path and a slow one: they were
		// byte-identical literals, and a field added to one and not the other
		// would apply only when the receiver app happened to already be
		// running.
		static nlohmann::json build_load(const LoadRequest& req, int request_id);
	};
