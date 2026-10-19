#pragma once

#include <nlohmann/json.hpp>

#include <functional>
#include <memory>
#include <string>
#include <utility>
#include <vector>

class WorkPool;

// What a logged-in client needs to sign a request.
struct Session {
	std::string server_key;
	std::string base_url;   // no trailing slash; /rest/ is appended
	std::string user;
	std::string salt;
	std::string token;
};

struct ApiResult {
	bool           ok=false;
	// Subsonic error code, 0 for transport and HTTP failures. 40 means the
	// credentials were refused.
	int            code=0;
	std::string    error;
	nlohmann::json body;   // the inside of "subsonic-response"
};

using Params = std::vector<std::pair<std::string, std::string>>;

class ApiClient {
	public:
		ApiClient(WorkPool& pool, Session session);

		const Session& session() const;

		// Runs on the pool; `done` is called on the main loop.
		void call(const std::string& endpoint, Params params,
		          std::function<void(const ApiResult&)> done);

		// For an endpoint that answers with a file rather than JSON, such as
		// getAlbumText. `done` is called on the main loop.
		void text(const std::string& endpoint, Params params,
		          std::function<void(bool ok, const std::string& body)> done);

		// For URLs handed to something else, such as covers and streams.
		std::string url(const std::string& endpoint, const Params& params) const;
		// The same for a Cast receiver: a getCastToken grant instead of the
		// account's credentials, which would let the receiver read the whole
		// library for as long as the password stands. An empty token keeps
		// the credentials, for a server without getCastToken.
		// `ext` is ".m3u8" for an HLS playlist, which is how a receiver
		// expects one to be spelled.
		std::string cast_url(const std::string& endpoint, const Params& params,
		                     const std::string& token, const char *ext=".view") const;

		// Derives a fresh salt and token from the password, and proves them
		// with ping and getUser before anything is stored. `done` is called on
		// the main loop, with the session filled in on success.
		static void login(WorkPool& pool, const std::string& url, const std::string& user,
		                  const std::string& password,
		                  std::function<void(const ApiResult&, const Session&)> done);

		// Blocking; for worker threads only.
		static ApiResult get_json(const Session& s, const std::string& endpoint, const Params& params);
		static bool      get_bytes(const std::string& url, std::string& body, std::string& error);
		// A GET with If-None-Match when `etag` is not empty, for a cache that
		// revalidates what it holds.
		struct Fetch {
			int         status=0;        // 0 when no answer came at all
			std::string body, etag, error;
			// From a refusal's Cache-Control max-age, which is how the server
			// says "there is nothing, and that will not change soon".
			int         miss_max_age=0;
		};
		static Fetch     get_if_changed(const std::string& url, const std::string& etag);
		// Fetches the first byte of a stream for its Content-Type. The server
		// builds a conversion whole before sending anything, so this can take
		// minutes, and is better done before a receiver starts its own clock.
		// `sent`, when given, gets the server's X-Gaindrive-Sent: what it
		// decided to send ("original", or a format and bitrate).
		static bool      probe_stream(const std::string& url, std::string& content_type,
		                              std::string& error, std::string *sent=nullptr);
		// X-Gaindrive-Sent alone, from a request dropped as soon as its
		// headers are in, so a conversion sent as it is made is not read
		// through. Empty when there is none, or no answer.
		static std::string sent_of(const std::string& url);

	private:
		WorkPool&                      pool_;
		std::shared_ptr<const Session> session_;
};

// Adds https:// when no scheme was typed, and drops a trailing slash or /rest.
std::string normalise_server_url(std::string url);

std::string random_hex(int nbytes);
