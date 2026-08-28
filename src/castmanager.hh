#pragma once

#include <string>
#include <vector>
#include <mutex>
#include <condition_variable>
#include <atomic>

#include <nlohmann/json.hpp>

class CastManager {
	public:
		struct CastDevice {
			std::string id;
			std::string name;
			// The `md` TXT record — "WiiM Amp Ultra", "Chromecast Ultra". The
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
			};

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
		// Every caller goes through here — listCastDevices and startCast's id
		// lookup both scan it — so the merge happening in one place is what lets
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
		// session state — deliberately not start(), which sets active_ and
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
		// four places at once — the call, the worker thread, the retry copy and
		// the JSON builder — so every field added as a parameter had to be
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
			};

		// Connect to the Chromecast, send LOAD, capture initial MEDIA_STATUS, then close.
		// The Chromecast fetches and plays independently; poll_loop maintains a separate
		// persistent connection that receives pushed status updates.
		// current_time tells the device where to start (seconds into the track).
		void load(const LoadRequest& req);

		// Stop Chromecast playback and exit cast mode.
		void stop();

		// Playback controls — each opens a fresh connection.
		void cast_pause();
		void cast_play();
		void cast_seek(float seconds);

		// Turn subtitle tracks on or off without reloading, by trackId; an
		// empty list means none. Returns false when the receiver has no media
		// session to edit yet, which is not an error — see the definition.
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

		// Block until the next status push from the Chromecast (or timeout_ms elapses).
		// Used by the SSE endpoint to stream updates to the browser.
		CastStatus wait_status(int timeout_ms = 15000);

		bool        active()          const { return active_; }
		std::string token()           const { return token_; }
		std::string get_device_id()   const { return device_.id; }
		std::string get_device_name() const { return device_.name; }
		float       last_known_time() const;
		bool        valid_token(const std::string& t) const
			{ return active_ && !token_.empty() && token_ == t; }

		// Incremented at the start of every load() call so content-provider
		// threads can detect that a new stream has started and exit promptly.
		int load_generation() const { return load_gen_.load(); }

	private:
		bool        active_ = false;
		CastDevice  device_;
		std::string token_;           // random token the Chromecast uses for stream auth

		std::atomic<int> load_gen_{0};

		mutable std::mutex         tid_mutex_;
		std::string                transport_id_;    // set in load(), needed for media commands

		mutable std::mutex         status_mutex_;
		std::condition_variable    status_cv_;       // notified on every status update
		CastStatus                 status_;
		float                      last_known_time_ = 0.0f; // current_time from last non-IDLE status

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

		// A LOAD the receiver refused, reported on the media namespace as an
		// ERROR/LOAD_FAILED rather than as a status.  Publishes the IDLE/ERROR
		// the receiver did not send and consumes the retry.
		void note_load_failure(int media_session_id);

		// Re-send a LOAD on a detached thread, abandoning it if the user has
		// asked for something else in the meantime.
		void spawn_retry(const LoadRequest& req, const char* why);

		// Open a fresh connection and send one media-namespace command.
		void send_media_cmd(const nlohmann::json& payload);

		// Background thread: maintains a persistent TLS connection to the Chromecast,
		// responds to PING heartbeats, and processes pushed MEDIA_STATUS messages.
		void poll_loop();

		// Worker spawned by load() — does the actual TLS connect + LOAD.
		// Checks load_gen_ against gen at each blocking step and aborts early if
		// a newer load() has been called.
		void load_worker(LoadRequest req, int gen);

		// The LOAD message itself.  One builder for both paths in load_worker:
		// they were byte-identical literals, and a field added to one and not
		// the other would apply only when the receiver app happened to already
		// be running.
		static nlohmann::json build_load(const LoadRequest& req, int request_id);
	};
