#include "wiimeq.hh"
#include "stamp.hh"
#include "textutil.hh"
#include "jsonread.hh"

#include <iostream>

#include <httplib.h>
#include <nlohmann/json.hpp>

namespace wiimeq {

const Band BANDS[10] = {
	{"band31hz",  "31"},
	{"band63hz",  "63"},
	{"band125hz", "125"},
	{"band250hz", "250"},
	{"band500hz", "500"},
	{"band1khz",  "1k"},
	{"band2khz",  "2k"},
	{"band4khz",  "4k"},
	{"band8khz",  "8k"},
	{"band16khz", "16k"},
	};

// One command, one reply body, or nullopt when the device did not answer.
static std::optional<std::string> command(const std::string& address,
                                          const std::string& cmd)
	{
	// Not CastDevice.port: 8009 is the Cast control port and has nothing to
	// do with this API.
	httplib::SSLClient cli(address, 443);
	// Every WiiM presents the same self-signed LinkPlay certificate
	// (CN=www.linkplay.com), which authenticates nothing: the user picked
	// this device off their own network, and nothing secret is sent to it.
	// Scoped to this client, never a global OpenSSL setting.
	cli.enable_server_certificate_verification(false);
	// A LAN round trip, on an httplib worker holding a browser request.
	cli.set_connection_timeout(3);
	cli.set_read_timeout(5);
	// The whole command is one query-parameter value and the device decodes
	// it as such. Encoding all of it is what keeps "R&B" from becoming
	// "EQLoad:R": an unencoded & ends the parameter.
	auto r = cli.Get(("/httpapi.asp?command=" + url_encode(cmd)).c_str());
	if (!r || r->status != 200) {
		std::cout << stamp() << "WiiM eq: " << cmd.substr(0, cmd.find(':'))
		          << " failed: "
		          << (r ? "HTTP " + std::to_string(r->status) : "no answer")
		          << std::endl;
		return std::nullopt;
		}
	// The device serves JSON as text/html; the content type means nothing.
	return r->body;
	}

// Success comes in two documented shapes, plain "OK" and {"status":"OK"},
// and where the documents differ, accept both: believing only one had every
// carried-out command reported as a failure (android/WIIM.md).
static bool is_ok(const std::string& body)
	{
	size_t a = body.find_first_not_of(" \t\r\n");
	if (a == std::string::npos) return false;
	size_t b = body.find_last_not_of(" \t\r\n");
	if (body.compare(a, b - a + 1, "OK") == 0) return true;
	auto j = nlohmann::json::parse(body, nullptr, false);
	return !j.is_discarded() && jstr(j, "status") == "OK";
	}

std::optional<State> state(const std::string& address)
	{
	// EQGetBand rather than EQGetStat: only this one names the loaded preset,
	// and a real Amp answers EQGetStat with {"status":"Failed"} anyway.
	auto body = command(address, "EQGetBand");
	if (!body) return std::nullopt;
	auto j = nlohmann::json::parse(*body, nullptr, false);
	if (j.is_discarded() || !j.is_object()) return std::nullopt;
	if (jstr(j, "status") == "Failed") return std::nullopt;

	State st;
	st.on     = jstr(j, "EQStat") == "On";
	st.preset = jstr(j, "Name");

	// Bands matched by param_name, all ten or nothing: a partial or garbled
	// array reads as "no faders" and the client degrades to the switch and
	// the preset list, while on/preset above stay valid.
	const auto& arr = jsub(j, "EQBand");
	if (arr.is_array()) {
		std::vector<int> vals(10, -1);
		for (const auto& e : arr) {
			std::string pn = jstr(e, "param_name");
			for (int i = 0; i < 10; i++)
				if (pn == BANDS[i].param) {
					int v = jint(e, "value");
					if (v >= LEVEL_MIN && v <= LEVEL_MAX) vals[i] = v;
					}
			}
		bool all = true;
		for (int v : vals) if (v < 0) all = false;
		if (all) st.bands = vals;
		}
	return st;
	}

std::optional<std::vector<std::string>> presets(const std::string& address)
	{
	auto body = command(address, "EQGetList");
	if (!body) return std::nullopt;
	auto j = nlohmann::json::parse(*body, nullptr, false);
	// A bare array on measured firmware; accept one wrapped in an object too,
	// because every other reply here is wrapped and the fallback (client-side)
	// hides its own failure.
	const nlohmann::json* arr = nullptr;
	if (j.is_array()) arr = &j;
	else if (j.is_object())
		for (const auto& [k, v] : j.items())
			if (v.is_array()) { arr = &v; break; }
	if (!arr) return std::nullopt;

	std::vector<std::string> names;
	for (const auto& e : *arr)
		if (e.is_string()) names.push_back(e.get<std::string>());
	if (names.empty()) return std::nullopt;
	return names;
	}

bool load_preset(const std::string& address, const std::string& name)
	{
	auto body = command(address, "EQLoad:" + name);
	if (!body || !is_ok(*body)) return false;
	// The documentation does not say whether EQLoad enables the equalizer;
	// sending EQOn makes the outcome the same either way, and picking "Rock"
	// while the switch is off must not be a silent no-op.
	return set_on(address, true);
	}

bool set_on(const std::string& address, bool on)
	{
	auto body = command(address, on ? "EQOn" : "EQOff");
	return body && is_ok(*body);
	}

bool set_bands(const std::string& address, const std::vector<int>& bands)
	{
	// Always the whole curve, so the outcome never depends on state the
	// caller last read rather than on what the user sees.
	if (bands.size() != 10) return false;
	for (int v : bands)
		if (v < LEVEL_MIN || v > LEVEL_MAX) return false;
	nlohmann::json arr = nlohmann::json::array();
	for (int i = 0; i < 10; i++)
		arr.push_back({{"index", i}, {"param_name", BANDS[i].param},
		               {"value", bands[i]}});
	auto body = command(address,
	                    "EQSetBand:" + nlohmann::json{{"EQBand", arr}}.dump());
	return body && is_ok(*body);
	}

	}
