#pragma once

#include <memory>
#include <string>
#include <vector>

class ApiClient;

// The servers with a session, one client each, in the order of
// servers.json. Everything that acts on an item asks here for the client of
// the item's own server, so a queue or a list can mix servers. Main thread
// only.
class Clients {
	public:
		// Null when that server has no session.
		std::shared_ptr<ApiClient> get(const std::string& server_key) const;
		const std::vector<std::shared_ptr<ApiClient>>& all() const;
		bool empty() const;

		// Replaces the client of the same server, or adds one at `pos`.
		void set(std::shared_ptr<ApiClient> api, size_t pos);
		void drop(const std::string& server_key);

	private:
		std::vector<std::shared_ptr<ApiClient>> list_;
};
