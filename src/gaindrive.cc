#include "gaindrive.hh"
#include "stamp.hh"

#include <iostream>
#include <map>
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

// Returns the name stripped of a leading article ("The ", "A ", …) for
// alphabetical index grouping.
static std::string sort_key(const std::string& name)
	{
	static const std::vector<std::string> articles = {
		"The ","A ","An ","El ","La ","Los ","Las ",
		"Le ","Les ","Die ","Das ","Ein ","Eine "
		};
	for (auto& art : articles)
		if (name.size() > art.size() && name.substr(0, art.size()) == art)
			return name.substr(art.size());
	return name;
	}

static std::string codec_to_mime(const std::string& codec)
	{
	static const std::map<std::string,std::string> m = {
		{"flac","audio/flac"}, {"mp3","audio/mpeg"},
		{"ogg","audio/ogg"},   {"opus","audio/ogg"},
		{"m4a","audio/mp4"},   {"aac","audio/aac"},
		{"wav","audio/wav"},   {"wma","audio/x-ms-wma"},
		};
	auto it = m.find(codec);
	return it != m.end() ? it->second : "application/octet-stream";
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

	// getIndexes — all artists grouped by first letter.
	server_.Get("/rest/getIndexes.view", [this](const httplib::Request& req,
	                                             httplib::Response& res) {
		if (!check_auth(req, res, store_)) return;

		auto artists = store_.get_artist_dirs();

		// Sort by article-stripped name, then group by first letter.
		std::sort(artists.begin(), artists.end(),
			[](const MediaStore::ArtistDir& a, const MediaStore::ArtistDir& b) {
				return sort_key(a.name) < sort_key(b.name);
				});

		// Build map: letter → list of artist XML strings.
		std::map<std::string, std::string> buckets;
		for (auto& a : artists) {
			std::string key = sort_key(a.name);
			std::string letter = key.empty() ? "#"
			                   : std::isalpha((unsigned char)key[0])
			                     ? std::string(1, (char)std::toupper((unsigned char)key[0]))
			                     : "#";
			buckets[letter] +=
				"<artist id=\"" + std::to_string(a.id) + "\""
				" name=\""      + a.name                + "\"/>";
			}

		std::string inner =
			"<indexes lastModified=\"0\""
			" ignoredArticles=\"The El La Los Las Le Les A An Die Das Ein Eine\">";
		for (auto& [letter, entries] : buckets)
			inner += "<index name=\"" + letter + "\">" + entries + "</index>";
		inner += "</indexes>";

		res.set_content(subsonic_ok(inner), "application/xml");
		});

	// getMusicDirectory — contents of a folder (album dirs or song files).
	server_.Get("/rest/getMusicDirectory.view", [this](const httplib::Request& req,
	                                                    httplib::Response& res) {
		if (!check_auth(req, res, store_)) return;

		auto it = req.params.find("id");
		if (it == req.params.end()) {
			res.set_content(subsonic_error(10, "Required parameter missing: id."),
			                "application/xml");
			return;
			}

		int folder_id = std::stoi(it->second);
		auto dir = store_.get_directory(folder_id);
		if (!dir) {
			res.set_content(subsonic_error(70, "Directory not found."), "application/xml");
			return;
			}

		std::string inner = "<directory id=\"" + std::to_string(dir->id) + "\""
			" name=\"" + dir->name + "\"";
		if (dir->parent_id >= 0)
			inner += " parent=\"" + std::to_string(dir->parent_id) + "\"";
		inner += ">";

		for (auto& c : dir->children) {
			inner += "<child id=\""     + std::to_string(c.id)        + "\""
			         " parent=\""       + std::to_string(c.parent_id) + "\""
			         " isDir=\""        + (c.is_dir ? "true" : "false") + "\""
			         " title=\""        + c.title                     + "\""
			         " artist=\""       + c.artist                    + "\""
			         " album=\""        + c.album                     + "\"";
			if (!c.is_dir) {
				inner += " track=\""       + std::to_string(c.track_number) + "\""
				         " discNumber=\""  + std::to_string(c.disc_number)  + "\""
				         " year=\""        + std::to_string(c.year)         + "\""
				         " genre=\""       + c.genre                        + "\""
				         " size=\""        + std::to_string(c.file_size)    + "\""
				         " contentType=\"" + codec_to_mime(c.codec)         + "\""
				         " suffix=\""      + c.codec                        + "\""
				         " duration=\""    + std::to_string((int)c.duration) + "\""
				         " bitRate=\""     + std::to_string(c.bitrate)      + "\""
				         " path=\""        + c.path                         + "\"";
				}
			inner += "/>";
			}

		inner += "</directory>";
		res.set_content(subsonic_ok(inner), "application/xml");
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
