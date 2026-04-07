#pragma once

#include <string>
#include <vector>

class CastManager {
	public:
		struct CastDevice {
			std::string id;
			std::string name;
			std::string address;
			int         port = 8009;
			};

		// Scan for Chromecast devices via mDNS for timeout_ms milliseconds.
		std::vector<CastDevice> discover(int timeout_ms);

		// Enter cast mode: store device and generate a single-use stream token.
		bool start(const CastDevice& device);

		// Lazy-connect to the Chromecast and tell it to load url.
		// Returns after sending LOAD; the Chromecast plays independently.
		void load(const std::string& url, const std::string& mime_type);

		// Stop Chromecast playback and exit cast mode.
		void stop();

		bool        active()     const { return active_; }
		std::string token()      const { return token_; }
		bool        valid_token(const std::string& t) const
			{ return active_ && !token_.empty() && token_ == t; }

	private:
		bool        active_ = false;
		CastDevice  device_;
		std::string token_;   // random token the Chromecast uses for stream auth
	};
