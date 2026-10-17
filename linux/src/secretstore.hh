#pragma once

#include <optional>
#include <string>

struct ServerConfig;

// The Subsonic token pair for one server, kept in the desktop keyring via the
// Secret Service. The password itself is never stored: like the web client,
// login derives salt and token once and keeps those. The salt is per login
// rather than per request so cover URLs stay cacheable.
struct StoredToken {
	std::string salt;
	std::string token;
};

// All three block on D-Bus; an unlock prompt can make that take a while.
bool                       secret_save(const ServerConfig& server, const StoredToken& tok);
std::optional<StoredToken> secret_load(const ServerConfig& server);
void                       secret_clear(const ServerConfig& server);
