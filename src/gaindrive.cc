#include "gaindrive.hh"
#include "stamp.hh"

#include <iostream>
#include <thread>

// ---- Subsonic XML helpers ---------------------------------------------

static const std::string SUBSONIC_NS  = "http://subsonic.org/restapi";
static const std::string SUBSONIC_VER = "1.16.1";

static std::string subsonic_ok(const std::string& inner = "")
	{
	return "<?xml version=\"1.0\" encoding=\"UTF-8\"?>"
	       "<subsonic-response xmlns=\"" + SUBSONIC_NS + "\""
	       " status=\"ok\" version=\"" + SUBSONIC_VER + "\">"
	       + inner +
	       "</subsonic-response>";
	}

static std::string subsonic_error(int code, const std::string& msg)
	{
	return "<?xml version=\"1.0\" encoding=\"UTF-8\"?>"
	       "<subsonic-response xmlns=\"" + SUBSONIC_NS + "\""
	       " status=\"failed\" version=\"" + SUBSONIC_VER + "\">"
	       "<error code=\"" + std::to_string(code) + "\""
	       " message=\"" + msg + "\"/>"
	       "</subsonic-response>";
	}

// Extracts u/p/t/s params and validates auth. Writes error into res on failure.
static bool check_auth(const httplib::Request& req, httplib::Response& res,
                       MediaStore& store)
	{
	auto p  = req.params;
	auto qp = [&](const std::string& k) -> std::string {
		auto it = p.find(k);
		return it != p.end() ? it->second : "";
		};

	std::string u  = qp("u");
	std::string pw = qp("p");
	std::string t  = qp("t");
	std::string s  = qp("s");

	if (u.empty() || (pw.empty() && (t.empty() || s.empty()))) {
		res.set_content(subsonic_error(10, "Required parameter missing."),
		                "application/xml");
		return false;
		}

	if (!store.validate_auth(u, pw, t, s)) {
		res.set_content(subsonic_error(40, "Wrong username or password."),
		                "application/xml");
		return false;
		}

	return true;
	}

// ---- GainDrive --------------------------------------------------------

GainDrive::GainDrive(const std::string& db_path,
                     const std::string& music_root,
                     bool no_scan)
	: store_(db_path, music_root)
	{
	server_.set_logger([](const httplib::Request& req, const httplib::Response& res) {
		std::cout << stamp(req.remote_addr)
		          << req.method << " " << req.path
		          << " -> " << res.status << std::endl;
		});

	// ping — simplest possible auth check.
	server_.Get("/rest/ping.view", [this](const httplib::Request& req,
	                                      httplib::Response& res) {
		if (!check_auth(req, res, store_)) return;
		res.set_content(subsonic_ok(), "application/xml");
		});

	// getLicense — return a perpetually-valid dummy license.
	server_.Get("/rest/getLicense.view", [this](const httplib::Request& req,
	                                             httplib::Response& res) {
		if (!check_auth(req, res, store_)) return;
		res.set_content(subsonic_ok(
			"<license valid=\"true\""
			" email=\"gaindrive@example.com\""
			" licenseExpires=\"2099-01-01T00:00:00\"/>"),
			"application/xml");
		});

	// getUser — return info for the requested username.
	server_.Get("/rest/getUser.view", [this](const httplib::Request& req,
	                                          httplib::Response& res) {
		if (!check_auth(req, res, store_)) return;

		auto qp = [&](const std::string& k) {
			auto it = req.params.find(k);
			return it != req.params.end() ? it->second : "";
			};

		std::string target = qp("username");
		if (target.empty()) {
			res.set_content(subsonic_error(10, "Required parameter missing: username."),
			                "application/xml");
			return;
			}

		// Non-admin users may only retrieve their own record.
		std::string requester = qp("u");
		if (target != requester) {
			auto ri = store_.get_user(requester);
			if (!ri || !ri->is_admin) {
				res.set_content(subsonic_error(50, "User is not authorized for this operation."),
				                "application/xml");
				return;
				}
			}

		auto ui = store_.get_user(target);
		if (!ui) {
			res.set_content(subsonic_error(70, "User not found."), "application/xml");
			return;
			}

		std::string bv = ui->is_admin ? "true" : "false";
		std::string xml =
			"<user username=\"" + ui->username + "\""
			" email=\""         + ui->email    + "\""
			" scrobblingEnabled=\"false\""
			" adminRole=\""     + bv + "\""
			" settingsRole=\""  + bv + "\""
			" downloadRole=\"true\""
			" uploadRole=\"false\""
			" playlistRole=\"true\""
			" coverArtRole=\"true\""
			" commentRole=\"false\""
			" podcastRole=\"false\""
			" streamRole=\"true\""
			" jukeboxRole=\"false\""
			" shareRole=\"false\"/>";

		res.set_content(subsonic_ok(xml), "application/xml");
		});

	// Catch-all for endpoints not yet implemented.
	server_.Get("/rest/:endpoint", [](const httplib::Request&, httplib::Response& res) {
		res.set_content(subsonic_error(0, "Not implemented."), "application/xml");
		});

	if (!store_.has_users())
		std::cout << stamp()
		          << "WARNING: no users in database. "
		             "Create one with --add-user <name> --password <pass>."
		          << std::endl;

	if (!no_scan)
		std::thread([this]{ store_.scan(); }).detach();
	}

void GainDrive::listen(const std::string& host, int port)
	{
	std::cout << stamp() << "Listening on " << host << ":" << port << std::endl;
	server_.listen(host, port);
	}
