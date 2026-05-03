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
			std::string address;
			int         port = 8009;
			};

		struct CastStatus {
			std::string player_state = "IDLE";  // IDLE | PLAYING | PAUSED | BUFFERING
			float       current_time = 0;
			float       duration     = 0;
			int         media_session_id = 0;
			std::string idle_reason;            // FINISHED | INTERRUPTED | ERROR (when IDLE)
			};

		// Scan for Chromecast devices via mDNS for timeout_ms milliseconds.
		std::vector<CastDevice> discover(int timeout_ms);

		// Run discover() in a background thread and cache the results.
		// Returns immediately; call cached_devices() to read the result.
		void discover_background(int timeout_ms = 4000);

		// Return the last cached device list (populated by discover_background).
		std::vector<CastDevice> cached_devices() const;

		// Enter cast mode: store device and generate a single-use stream token.
		bool start(const CastDevice& device);

		// Connect to the Chromecast, send LOAD, capture initial MEDIA_STATUS, then close.
		// The Chromecast fetches and plays independently; poll_loop maintains a separate
		// persistent connection that receives pushed status updates.
		// current_time tells the device where to start (seconds into the track).
		void load(const std::string& url, const std::string& mime_type,
		          float current_time = 0.0f, double duration = 0.0);

		// Stop Chromecast playback and exit cast mode.
		void stop();

		// Playback controls — each opens a fresh connection.
		void cast_pause();
		void cast_play();
		void cast_seek(float seconds);

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

		mutable std::mutex         cache_mutex_;
		std::vector<CastDevice>    devices_cache_;  // last result of discover_background()

		std::atomic<bool>          poll_active_{false};

		// Parse a MEDIA_STATUS message and store the result in status_.
		void update_status(const nlohmann::json& msg);

		// Open a fresh connection and send one media-namespace command.
		void send_media_cmd(const nlohmann::json& payload);

		// Background thread: maintains a persistent TLS connection to the Chromecast,
		// responds to PING heartbeats, and processes pushed MEDIA_STATUS messages.
		void poll_loop();

		// Worker spawned by load() — does the actual TLS connect + LOAD.
		// Checks load_gen_ against gen at each blocking step and aborts early if
		// a newer load() has been called.
		void load_worker(std::string url, std::string mime, int gen,
		                 float current_time, double duration);
	};
