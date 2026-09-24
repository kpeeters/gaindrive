#pragma once

// The Subsonic response envelope, in both spellings, plus the query-parameter
// conversions every handler needs before it can trust a value.
//
// Split out of gaindrive.cc because every route group calls these:
// subsonic_error is reached from all nine, check_auth included, so leaving
// them behind would have meant the route files could not move at all.

#include <cstdint>
#include <functional>
#include <string>

#include <httplib.h>
#include <tinyxml2.h>
#include <nlohmann/json.hpp>

// The Subsonic API version this server claims. Also appended to the URLs the
// HLS playlist hands back, which is why it is here rather than private to
// subsonic.cc like the namespace and the server identity beside it.
inline constexpr const char* SUBSONIC_VER = "1.16.1";

// Sent on every outbound metadata request. One definition rather than the nine
// copies this used to be, so a version bump cannot leave some of them behind.
inline constexpr const char* USER_AGENT =
	"GainDrive/" GAINDRIVE_VERSION " (info@phi-sci.com)";

// Subsonic ids are strings in the API even though they are row ids here. Every
// id crossing the wire in JSON goes through this - the XML path renders
// attributes as text anyway, so it needs no equivalent.
std::string sid(int id);

// Numeric query params, without letting a malformed one escape the handler.
// std::stoi throws on garbage and on overflow; httplib turns that into a bare
// HTTP 500, which no Subsonic client can interpret - they expect a 200 with an
// <error> body.  Every request parameter goes through these; a bare std::stoi
// below this point reads a value the server itself produced.
int     to_int(const std::string& s, int def);
float   to_float(const std::string& s, float def);
int64_t to_int64(const std::string& s, int64_t def);

// SQLite CURRENT_TIMESTAMP formats as "YYYY-MM-DD HH:MM:SS" in UTC, but the
// API wants ISO 8601. Same instant, different spelling. Empty in, empty out,
// so callers can keep using emptiness to mean "absent".
std::string iso8601(const std::string& ts);

// Build a complete ok response, optionally populated by a callback.
std::string subsonic_ok(
	std::function<void(tinyxml2::XMLDocument&, tinyxml2::XMLElement*)> fn = {});
std::string subsonic_error(int code, const char* msg);

std::string subsonic_ok_json(std::function<void(nlohmann::json&)> fn = {});
std::string subsonic_error_json(int code, const char* msg);

// Returns "json" if the client requested JSON, otherwise "xml".
std::string fmt_of(const httplib::Request& req);
