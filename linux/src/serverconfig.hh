#pragma once

#include <string>
#include <vector>

// A server the user has logged in to. The credential itself is not here; it
// lives in the keyring (secretstore.hh), looked up by `key`.
struct ServerConfig {
	// Random and fixed when the server is added, so that ids keyed by it
	// survive the server moving to another address.
	std::string key;
	std::string url;
	std::string user;
	// What rows and the server picker call it; empty for the host, as on
	// the phones. A long URL would crowd every album row.
	std::string name;
};

// The host part of a server URL.
std::string server_host(const std::string& url);

// servers.json in the config directory. The order is the user's: the first
// server's copy of a merged artist or album is the one shown.
std::vector<ServerConfig> load_servers();
void                      save_servers(const std::vector<ServerConfig>& servers);
