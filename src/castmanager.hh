#pragma once

#include <string>
#include <vector>
#include <thread>
#include <mutex>
#include <atomic>

#include <openssl/ssl.h>
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
			};

		// Scan for Chromecast devices via mDNS for timeout_ms milliseconds.
		std::vector<CastDevice> discover(int timeout_ms);

		// Enter cast mode: store device and generate a single-use stream token.
		bool start(const CastDevice& device);

		// Lazy-connect to the Chromecast and tell it to load url.
		// Keeps the connection open in a background thread to receive MEDIA_STATUS.
		void load(const std::string& url, const std::string& mime_type);

		// Stop Chromecast playback and exit cast mode.
		void stop();

		// Playback controls — send commands over a fresh connection.
		void cast_pause();
		void cast_play();
		void cast_seek(float seconds);

		// Thread-safe read of the latest MEDIA_STATUS from the monitor thread.
		CastStatus get_status() const;

		bool        active()     const { return active_; }
		std::string token()      const { return token_; }
		bool        valid_token(const std::string& t) const
			{ return active_ && !token_.empty() && token_ == t; }

		~CastManager();

	private:
		bool        active_ = false;
		CastDevice  device_;
		std::string token_;           // random token the Chromecast uses for stream auth
		std::string transport_id_;    // set in load(), needed for media commands

		mutable std::mutex   status_mutex_;
		CastStatus           status_;
		std::thread          monitor_;
		std::atomic<bool>    stop_monitor_{false};

		// Runs in monitor_; owns ssl/ctx/sock and cleans up on exit.
		void run_monitor(SSL* ssl, SSL_CTX* ctx, int sock);

		// Open a fresh connection and send one media-namespace command.
		void send_media_cmd(const nlohmann::json& payload);
	};
