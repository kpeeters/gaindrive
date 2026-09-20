#include "subsonic.hh"

using namespace tinyxml2;

static const char* SUBSONIC_NS = "http://subsonic.org/restapi";

// OpenSubsonic requires a server that sets openSubsonic=true to also identify
// itself: `type` is the implementation, `serverVersion` is our own version as
// distinct from the API version above.
static const char* SERVER_TYPE    = "gaindrive";
static const char* SERVER_VERSION = GAINDRIVE_VERSION;

std::string sid(int id)
	{
	return std::to_string(id);
	}

int to_int(const std::string& s, int def)
	{
	if (s.empty()) return def;
	try { return std::stoi(s); } catch (...) { return def; }
	}

float to_float(const std::string& s, float def)
	{
	if (s.empty()) return def;
	try { return std::stof(s); } catch (...) { return def; }
	}

int64_t to_int64(const std::string& s, int64_t def)
	{
	if (s.empty()) return def;
	try { return std::stoll(s); } catch (...) { return def; }
	}

std::string iso8601(const std::string& ts)
	{
	if (ts.empty()) return ts;
	std::string s = ts;
	if (s.size() > 10 && s[10] == ' ') s[10] = 'T';
	if (s.back() != 'Z') s += 'Z';
	return s;
	}

// Creates a <subsonic-response> root element inside doc and returns it.
static XMLElement* make_root(XMLDocument& doc, const char* status)
	{
	doc.InsertEndChild(doc.NewDeclaration());
	auto* root = doc.NewElement("subsonic-response");
	root->SetAttribute("xmlns",        SUBSONIC_NS);
	root->SetAttribute("status",        status);
	root->SetAttribute("version",       SUBSONIC_VER);
	root->SetAttribute("type",          SERVER_TYPE);
	root->SetAttribute("serverVersion", SERVER_VERSION);
	root->SetAttribute("openSubsonic",  "true");
	doc.InsertEndChild(root);
	return root;
	}

static std::string to_string(XMLDocument& doc)
	{
	XMLPrinter printer;
	doc.Print(&printer);
	return printer.CStr();
	}

std::string subsonic_ok(std::function<void(XMLDocument&, XMLElement*)> fn)
	{
	XMLDocument doc;
	auto* root = make_root(doc, "ok");
	if (fn) fn(doc, root);
	return to_string(doc);
	}

std::string subsonic_error(int code, const char* msg)
	{
	XMLDocument doc;
	auto* root = make_root(doc, "failed");
	auto* err  = doc.NewElement("error");
	err->SetAttribute("code",    code);
	err->SetAttribute("message", msg);
	root->InsertEndChild(err);
	return to_string(doc);
	}

// ---- Subsonic JSON helpers --------------------------------------------

std::string subsonic_ok_json(std::function<void(nlohmann::json&)> fn)
	{
	nlohmann::json r;
	r["status"]        = "ok";
	r["version"]       = SUBSONIC_VER;
	r["type"]          = SERVER_TYPE;
	r["serverVersion"] = SERVER_VERSION;
	r["openSubsonic"]  = true;
	if (fn) fn(r);
	nlohmann::json j;
	j["subsonic-response"] = r;
	return j.dump();
	}

std::string subsonic_error_json(int code, const char* msg)
	{
	nlohmann::json j;
	j["subsonic-response"]["status"]           = "failed";
	j["subsonic-response"]["version"]          = SUBSONIC_VER;
	j["subsonic-response"]["type"]             = SERVER_TYPE;
	j["subsonic-response"]["serverVersion"]    = SERVER_VERSION;
	j["subsonic-response"]["openSubsonic"]     = true;
	j["subsonic-response"]["error"]["code"]    = code;
	j["subsonic-response"]["error"]["message"] = msg;
	return j.dump();
	}

std::string fmt_of(const httplib::Request& req)
	{
	auto it = req.params.find("f");
	return (it != req.params.end() && it->second == "json") ? "json" : "xml";
	}
