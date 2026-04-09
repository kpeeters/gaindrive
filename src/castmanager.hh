#pragma once

#include <string>
#include <vector>
#include <mutex>

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

		// Connect to the Chromecast, send LOAD, capture initial MEDIA_STATUS, then close.
		// The Chromecast fetches and plays independently; no persistent connection is kept.
		void load(const std::string& url, const std::string& mime_type);

		// Stop Chromecast playback and exit cast mode.
		void stop();

		// Playback controls — each opens a fresh connection.
		void cast_pause();
		void cast_play();
		void cast_seek(float seconds);

		// Open a fresh connection, send GET_STATUS, update the cached status, return it.
		// Serialised by fetch_mutex_ so concurrent getCastStatus calls don't pile up.
		CastStatus fetch_status();

		// Return the last cached status (used internally for mediaSessionId).
		CastStatus get_status() const;

		bool        active()     const { return active_; }
		std::string token()      const { return token_; }
		bool        valid_token(const std::string& t) const
			{ return active_ && !token_.empty() && token_ == t; }

	private:
		bool        active_ = false;
		CastDevice  device_;
		std::string token_;           // random token the Chromecast uses for stream auth
		std::string transport_id_;    // set in load(), needed for media commands

		mutable std::mutex   status_mutex_;
		CastStatus           status_;
		std::mutex           fetch_mutex_;   // one fetch_status() at a time

		// Parse a MEDIA_STATUS message and store the result in status_.
		void update_status(const nlohmann::json& msg);

		// Open a fresh connection and send one media-namespace command.
		void send_media_cmd(const nlohmann::json& payload);
	};
