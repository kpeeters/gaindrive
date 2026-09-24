#include "gaindrive.hh"
#include "subsonic.hh"
#include "authz.hh"
#include "stamp.hh"
#include "textutil.hh"
#include "netaddr.hh"
#include "apientry.hh"
#include "countingpool.hh"
#include "streamer.hh"

#include <filesystem>
#include <fstream>

#include <iostream>

#include <unistd.h>

#include <tinyxml2.h>
#include <nlohmann/json.hpp>

using namespace tinyxml2;

// Resident set size from /proc/self/statm; -1 where there is no proc
// filesystem, and the field is then omitted rather than reported as zero.
static long long rss_bytes()
	{
	std::ifstream f("/proc/self/statm");
	long long pages = 0, resident = 0;
	if (!(f >> pages >> resident)) return -1;
	return resident * sysconf(_SC_PAGE_SIZE);
	}

void GainDrive::routes_system()
	{
	// ping, carrying one gaindrive field: `localNetwork`, whether this request
	// reached the server from a network the server is itself attached to.
	//
	// It rides here rather than beside castRole on getUser because it is a
	// fact about the *request*, not about the account - the same person is on
	// the home network in the morning and not in the afternoon. Folding it
	// into castRole would be worse than untidy: web/app.js reads that field
	// into the admin edit form and writes it back, so an admin editing their
	// own account from abroad would silently revoke their own role.

	server_.Get("/rest/ping.view", [this](const httplib::Request& req,
	                                      httplib::Response& res) {
		if (!check_auth(req, res, store_)) return;
		bool use_json = (fmt_of(req) == "json");
		const bool local = client_is_local(req);
		std::string body = use_json
			? subsonic_ok_json([&](nlohmann::json& r) {
				r["localNetwork"] = local;
				})
			: subsonic_ok([&](XMLDocument&, XMLElement* root) {
				root->SetAttribute("localNetwork", local);
				});
		res.set_content(body, use_json ? "application/json" : "application/xml");
		});

	// getOpenSubsonicExtensions - no auth required; clients call this before login.
	server_.Get("/rest/getOpenSubsonicExtensions.view",
	            [this](const httplib::Request& req, httplib::Response& res) {
		bool use_json = (fmt_of(req) == "json");
		std::string body;
		if (use_json)
			body = subsonic_ok_json([](nlohmann::json& r) {
				// One version until the first public release: everything the
				// extension offers is version 1. A new *endpoint* will earn a
				// new number, since a client cannot discover one without being
				// told; a new optional parameter on an endpoint an existing
				// version already names will not, since a client that ignores
				// it gets exactly what it got before. doc/api.toml is the
				// contract.
				//
				// transcodeOffset is the standard extension meaning no more
				// than that stream honours timeOffset for audio, which it has
				// always done - so it names behaviour rather than adding any.
				r["openSubsonicExtensions"] = {
					{{"name", "gaindrive"},
					 {"versions", nlohmann::json::array({1})}},
					{{"name", "transcodeOffset"},
					 {"versions", nlohmann::json::array({1})}}};
				});
		else
			body = subsonic_ok([](XMLDocument& doc, XMLElement* root) {
				auto* exts = doc.NewElement("openSubsonicExtensions");
				for (const char* name : {"gaindrive", "transcodeOffset"}) {
					auto* ext = doc.NewElement("extension");
					ext->SetAttribute("name", name);
					ext->SetAttribute("versions", "1");
					exts->InsertEndChild(ext);
					}
				root->InsertEndChild(exts);
				});
		res.set_content(body, use_json ? "application/json" : "application/xml");
		});

	// getLicense - perpetually-valid dummy.
	server_.Get("/rest/getLicense.view", [this](const httplib::Request& req,
	                                             httplib::Response& res) {
		if (!check_auth(req, res, store_)) return;
		bool use_json = (fmt_of(req) == "json");
		std::string body;
		if (use_json)
			body = subsonic_ok_json([](nlohmann::json& r) {
				r["license"] = {
					{"valid",          true},
					{"email",          "gaindrive@example.com"},
					{"licenseExpires", "2099-01-01T00:00:00"}
					};
				});
		else
			body = subsonic_ok([](XMLDocument& doc, XMLElement* root) {
				auto* lic = doc.NewElement("license");
				lic->SetAttribute("valid",          "true");
				lic->SetAttribute("email",          "gaindrive@example.com");
				lic->SetAttribute("licenseExpires", "2099-01-01T00:00:00");
				root->InsertEndChild(lic);
				});
		res.set_content(body, use_json ? "application/json" : "application/xml");
		});

	// ---- Media library scanning (standard, Subsonic 1.15.0) ---------------
	//
	// Both take no parameters and answer with the same <scanStatus> element.
	// `scanning` is a real boolean and `count` a real number in JSON: a strict
	// client throws on a type mismatch before any of the response is usable.
	auto scan_status_body = [this](bool use_json) {
		auto st = store_.scan_status();
		if (use_json)
			return subsonic_ok_json([&](nlohmann::json& r) {
				r["scanStatus"] = {{"scanning", st.scanning},
				                   {"count",    st.count}};
				});
		return subsonic_ok([&](XMLDocument& doc, XMLElement* root) {
			auto* el = doc.NewElement("scanStatus");
			el->SetAttribute("scanning", st.scanning);
			el->SetAttribute("count",    static_cast<int64_t>(st.count));
			root->InsertEndChild(el);
			});
		};

	server_.Get("/rest/getScanStatus.view",
	            [this, scan_status_body](const httplib::Request& req,
	                                     httplib::Response& res) {
		if (!check_auth(req, res, store_)) return;
		bool use_json = (fmt_of(req) == "json");
		res.set_content(scan_status_body(use_json),
		                use_json ? "application/json" : "application/xml");
		});

	// startScan - admin only, matching the rule that settingsRole mirrors
	// adminRole. Note --no-scan suppresses only the scan at start-up; asking
	// for one explicitly still works.
	server_.Get("/rest/startScan.view",
	            [this, scan_status_body](const httplib::Request& req,
	                                     httplib::Response& res) {
		if (!check_auth(req, res, store_)) return;
		bool use_json = (fmt_of(req) == "json");

		auto ui = store_.get_user(req.get_param_value("u"));
		if (!ui || !ui->is_admin) {
			const char* msg = "Scanning the library requires admin role.";
			res.set_content(use_json ? subsonic_error_json(50, msg)
			                         : subsonic_error(50, msg),
			                use_json ? "application/json" : "application/xml");
			return;
			}

		// Already scanning: report that rather than starting a second walk
		// over the same tree. Nothing would break - the scan is idempotent and
		// SQLite serialises the writes - but it would double the I/O and make
		// `count` jump about between two walks sharing one counter.
		if (!store_.scan_status().scanning)
			std::thread([this]{
				try { store_.scan(); }
				catch (const std::exception& e) {
					std::cout << stamp() << "Scan aborted: " << e.what()
					          << std::endl;
					}
				catch (...) {
					std::cout << stamp() << "Scan aborted: unknown exception"
					          << std::endl;
					}
				}).detach();

		res.set_content(scan_status_body(use_json),
		                use_json ? "application/json" : "application/xml");
		});

	// startInfoLookup - queue an online lookup for everything that has no
	// words yet. A gaindrive extension, and startScan's sibling: the same
	// shape, the same admin gate, the same "it is running, come back later"
	// answer.
	//
	// Admin because it rewrites the shared library and because it spends hours
	// of this server's single MusicBrainz budget - one artist or album every
	// two seconds - which is not a thing one account should be able to do to
	// everybody else's browsing.
	//
	// It reports what it queued and nothing further. There is no status
	// endpoint and no way to stop it short of a restart; the log is the
	// progress display for now.
	server_.Get("/rest/startInfoLookup.view", [this](const httplib::Request& req,
	                                                  httplib::Response& res) {
		if (!check_auth(req, res, store_)) return;
		bool use_json = (fmt_of(req) == "json");
		auto err = [&](int code, const char* msg) {
			res.set_content(use_json ? subsonic_error_json(code, msg)
			                         : subsonic_error(code, msg),
			                use_json ? "application/json" : "application/xml");
			};
		auto qp = [&](const std::string& k) -> std::string {
			auto it = req.params.find(k); return it != req.params.end() ? it->second : "";
			};
		auto ri = store_.get_user(qp("u"));
		if (!ri || !ri->is_admin) { err(50, "User is not authorized for this operation."); return; }

		// An unrecognised value is a client bug rather than a request to do
		// everything: silently promoting a typo to "all" would start an
		// overnight pass nobody asked for.
		std::string what = qp("what");
		if (what.empty()) what = "all";
		if (what != "all" && what != "artists" && what != "albums") {
			err(0, "Invalid value for what: expected artists, albums or all.");
			return;
			}

		auto n = lookup_seed_missing_info(what != "albums", what != "artists");
		std::cout << stamp() << "startInfoLookup: queued " << n.artists
		          << " artist(s) and " << n.albums << " album(s)" << std::endl;

		std::string body;
		if (use_json)
			body = subsonic_ok_json([&n](nlohmann::json& r) {
				r["infoLookup"] = {{"artistsQueued", n.artists},
				                   {"albumsQueued",  n.albums}};
				});
		else
			body = subsonic_ok([&n](XMLDocument& doc, XMLElement* root) {
				auto* el = doc.NewElement("infoLookup");
				el->SetAttribute("artistsQueued", n.artists);
				el->SetAttribute("albumsQueued",  n.albums);
				root->InsertEndChild(el);
				});
		res.set_content(body, use_json ? "application/json" : "application/xml");
		});

	// getUser
	server_.Get("/rest/getUser.view", [this](const httplib::Request& req,
	                                          httplib::Response& res) {
		if (!check_auth(req, res, store_)) return;

		auto qp = [&](const std::string& k) {
			auto it = req.params.find(k);
			return it != req.params.end() ? it->second : "";
			};

		bool use_json = (fmt_of(req) == "json");
		auto err = [&](int code, const char* msg) {
			if (use_json)
				res.set_content(subsonic_error_json(code, msg), "application/json");
			else
				res.set_content(subsonic_error(code, msg),      "application/xml");
			};

		std::string target = qp("username");
		if (target.empty()) { err(10, "Required parameter missing: username."); return; }

		std::string requester = qp("u");
		if (target != requester) {
			auto ri = store_.get_user(requester);
			if (!ri || !ri->is_admin) {
				err(50, "User is not authorized for this operation.");
				return;
				}
			}

		auto ui = store_.get_user(target);
		if (!ui) { err(70, "User not found."); return; }

		std::string body;
		if (use_json)
			body = subsonic_ok_json([&ui](nlohmann::json& r) {
				r["user"] = {
					{"username",          ui->username},
					{"email",             ui->email},
					{"scrobblingEnabled", false},
					{"adminRole",         ui->is_admin},
					{"settingsRole",      ui->is_admin},
					{"downloadRole",      true},
					{"uploadRole",        ui->upload_allowed},
					{"playlistRole",      true},
					{"coverArtRole",      true},
					{"commentRole",       false},
					{"podcastRole",       false},
					{"streamRole",        true},
					{"jukeboxRole",       false},
					{"shareRole",         false},
					{"maxBitRate",        ui->max_bitrate},
					{"disabled",          ui->disabled},
					{"castRole",          ui->cast_allowed}
					};
				});
		else
			body = subsonic_ok([&ui](XMLDocument& doc, XMLElement* root) {
				auto* u = doc.NewElement("user");
				u->SetAttribute("username",          ui->username.c_str());
				u->SetAttribute("email",             ui->email.c_str());
				u->SetAttribute("scrobblingEnabled", false);
				u->SetAttribute("adminRole",         ui->is_admin);
				u->SetAttribute("settingsRole",      ui->is_admin);
				u->SetAttribute("downloadRole",      true);
				u->SetAttribute("uploadRole",        ui->upload_allowed);
				u->SetAttribute("playlistRole",      true);
				u->SetAttribute("coverArtRole",      true);
				u->SetAttribute("commentRole",       false);
				u->SetAttribute("podcastRole",       false);
				u->SetAttribute("streamRole",        true);
				u->SetAttribute("jukeboxRole",       false);
				u->SetAttribute("shareRole",         false);
				u->SetAttribute("maxBitRate",        ui->max_bitrate);
				u->SetAttribute("disabled",          ui->disabled);
				u->SetAttribute("castRole",          ui->cast_allowed);
				root->InsertEndChild(u);
				});
		res.set_content(body, use_json ? "application/json" : "application/xml");
		});

	// getUsers - returns all users; admin only.
	server_.Get("/rest/getUsers.view", [this](const httplib::Request& req,
	                                           httplib::Response& res) {
		if (!check_auth(req, res, store_)) return;
		bool use_json = (fmt_of(req) == "json");
		auto err = [&](int code, const char* msg) {
			res.set_content(use_json ? subsonic_error_json(code, msg)
			                         : subsonic_error(code, msg),
			                use_json ? "application/json" : "application/xml");
			};

		auto ri = store_.get_user(req.params.find("u")->second);
		if (!ri || !ri->is_admin) { err(50, "User is not authorized for this operation."); return; }

		auto users = store_.list_users();
		std::string body;
		if (use_json)
			body = subsonic_ok_json([&users](nlohmann::json& r) {
				nlohmann::json arr = nlohmann::json::array();
				for (const auto& u : users)
					arr.push_back({
						{"username",          u.username},
						{"email",             u.email},
						{"scrobblingEnabled", false},
						{"adminRole",         u.is_admin},
						{"settingsRole",      u.is_admin},
						{"downloadRole",      true},
						{"uploadRole",        u.upload_allowed},
						{"playlistRole",      true},
						{"coverArtRole",      true},
						{"commentRole",       false},
						{"podcastRole",       false},
						{"streamRole",        true},
						{"jukeboxRole",       false},
						{"shareRole",         false},
						{"maxBitRate",        u.max_bitrate},
						{"disabled",          u.disabled},
				{"castRole",          u.cast_allowed}
						});
				r["users"] = {{"user", arr}};
				});
		else
			body = subsonic_ok([&users](XMLDocument& doc, XMLElement* root) {
				auto* us = doc.NewElement("users");
				for (const auto& u : users) {
					auto* ue = doc.NewElement("user");
					ue->SetAttribute("username",          u.username.c_str());
					ue->SetAttribute("email",             u.email.c_str());
					ue->SetAttribute("scrobblingEnabled", false);
					ue->SetAttribute("adminRole",         u.is_admin);
					ue->SetAttribute("settingsRole",      u.is_admin);
					ue->SetAttribute("downloadRole",      true);
					ue->SetAttribute("uploadRole",        u.upload_allowed);
					ue->SetAttribute("playlistRole",      true);
					ue->SetAttribute("coverArtRole",      true);
					ue->SetAttribute("commentRole",       false);
					ue->SetAttribute("podcastRole",       false);
					ue->SetAttribute("streamRole",        true);
					ue->SetAttribute("jukeboxRole",       false);
					ue->SetAttribute("shareRole",         false);
					ue->SetAttribute("maxBitRate",        u.max_bitrate);
					ue->SetAttribute("disabled",          u.disabled);
					ue->SetAttribute("castRole",          u.cast_allowed);
					us->InsertEndChild(ue);
					}
				root->InsertEndChild(us);
				});
		res.set_content(body, use_json ? "application/json" : "application/xml");
		});

	// createUser - creates a new user; admin only.
	server_.Get("/rest/createUser.view", [this](const httplib::Request& req,
	                                             httplib::Response& res) {
		if (!check_auth(req, res, store_)) return;
		bool use_json = (fmt_of(req) == "json");
		auto err = [&](int code, const char* msg) {
			res.set_content(use_json ? subsonic_error_json(code, msg)
			                         : subsonic_error(code, msg),
			                use_json ? "application/json" : "application/xml");
			};
		auto qp = [&](const std::string& k) -> std::string {
			auto it = req.params.find(k); return it != req.params.end() ? it->second : "";
			};

		auto ri = store_.get_user(qp("u"));
		if (!ri || !ri->is_admin) { err(50, "User is not authorized for this operation."); return; }

		std::string username = qp("username");
		// enc: decoded before storing - validate_auth decodes it on the way
		// in, so storing the encoded spelling stores a different password.
		auto decoded = MediaStore::decode_enc_password(qp("password"));
		if (!decoded) { err(10, "Malformed enc: password."); return; }
		std::string password = *decoded;
		if (username.empty()) { err(10, "Required parameter missing: username."); return; }
		if (password.empty()) { err(10, "Required parameter missing: password."); return; }
		if (!MediaStore::valid_username(username)) {
			err(0, "Username may contain only letters, digits, '.', '_' and '-'.");
			return;
			}

		bool is_admin       = (qp("adminRole")  == "true");
		bool upload_allowed = (qp("uploadRole") == "true");
		bool disabled       = (qp("disabled")   == "true");
		bool cast_allowed   = (qp("castRole")   == "true");
		int  max_bitrate    = 0;
		if (!qp("maxBitRate").empty()) max_bitrate = to_int(qp("maxBitRate"), 0);

		if (!store_.add_user(username, password, is_admin)) {
			err(0, "User already exists.");
			return;
			}
		store_.update_user(username, "", qp("email"), is_admin, max_bitrate, upload_allowed, disabled, cast_allowed);

		std::string body = use_json ? subsonic_ok_json() : subsonic_ok();
		res.set_content(body, use_json ? "application/json" : "application/xml");
		});

	// updateUser - updates an existing user; admin only.
	server_.Get("/rest/updateUser.view", [this](const httplib::Request& req,
	                                             httplib::Response& res) {
		if (!check_auth(req, res, store_)) return;
		bool use_json = (fmt_of(req) == "json");
		auto err = [&](int code, const char* msg) {
			res.set_content(use_json ? subsonic_error_json(code, msg)
			                         : subsonic_error(code, msg),
			                use_json ? "application/json" : "application/xml");
			};
		auto qp = [&](const std::string& k) -> std::string {
			auto it = req.params.find(k); return it != req.params.end() ? it->second : "";
			};

		auto ri = store_.get_user(qp("u"));
		if (!ri || !ri->is_admin) { err(50, "User is not authorized for this operation."); return; }

		std::string username = qp("username");
		if (username.empty()) { err(10, "Required parameter missing: username."); return; }

		// Fetch current values so unspecified params keep their existing value.
		auto existing = store_.get_user(username);
		if (!existing) { err(70, "User not found."); return; }

		bool is_admin       = qp("adminRole").empty()  ? existing->is_admin       : (qp("adminRole")  == "true");
		bool upload_allowed = qp("uploadRole").empty() ? existing->upload_allowed : (qp("uploadRole") == "true");
		bool disabled       = qp("disabled").empty()   ? existing->disabled       : (qp("disabled")   == "true");
		bool cast_allowed   = qp("castRole").empty()   ? existing->cast_allowed   : (qp("castRole")   == "true");
		int  max_bitrate    = qp("maxBitRate").empty()  ? existing->max_bitrate    : std::max(0, to_int(qp("maxBitRate"), existing->max_bitrate));
		std::string email   = qp("email").empty()       ? existing->email          : qp("email");
		// enc: decoded before storing, as in createUser.
		auto pw_dec = MediaStore::decode_enc_password(qp("password"));
		if (!pw_dec) { err(10, "Malformed enc: password."); return; }
		std::string pw      = *pw_dec;

		// An admin cannot disable their own account.
		if (username == qp("u") && disabled)
			{ err(0, "You cannot disable your own account."); return; }

		store_.update_user(username, pw, email, is_admin, max_bitrate, upload_allowed, disabled, cast_allowed);

		std::string body = use_json ? subsonic_ok_json() : subsonic_ok();
		res.set_content(body, use_json ? "application/json" : "application/xml");
		});

	// changePassword - change a user's password; admins can change any user's,
	// regular users can only change their own.
	server_.Get("/rest/changePassword.view", [this](const httplib::Request& req,
	                                                 httplib::Response& res) {
		if (!check_auth(req, res, store_)) return;
		bool use_json = (fmt_of(req) == "json");
		auto err = [&](int code, const char* msg) {
			res.set_content(use_json ? subsonic_error_json(code, msg)
			                         : subsonic_error(code, msg),
			                use_json ? "application/json" : "application/xml");
			};
		auto qp = [&](const std::string& k) -> std::string {
			auto it = req.params.find(k); return it != req.params.end() ? it->second : "";
			};

		std::string requester = qp("u");
		std::string target    = qp("username");
		// enc: decoded before storing, as in createUser.
		auto decoded = MediaStore::decode_enc_password(qp("password"));
		if (!decoded) { err(10, "Malformed enc: password."); return; }
		std::string password  = *decoded;
		if (target.empty())   { err(10, "Required parameter missing: username."); return; }
		if (password.empty()) { err(10, "Required parameter missing: password."); return; }

		if (target != requester) {
			auto ri = store_.get_user(requester);
			if (!ri || !ri->is_admin) { err(50, "User is not authorized for this operation."); return; }
			}

		auto existing = store_.get_user(target);
		if (!existing) { err(70, "User not found."); return; }

		store_.update_user(target, password, existing->email, existing->is_admin,
		                   existing->max_bitrate, existing->upload_allowed, existing->disabled,
		                   existing->cast_allowed);

		std::string body = use_json ? subsonic_ok_json() : subsonic_ok();
		res.set_content(body, use_json ? "application/json" : "application/xml");
		});

	// getServerSettings / saveServerSettings - admin-only server configuration.
	server_.Get("/rest/getServerSettings.view", [this](const httplib::Request& req,
	                                                    httplib::Response& res) {
		if (!check_auth(req, res, store_)) return;
		bool use_json = (fmt_of(req) == "json");
		auto err = [&](int code, const char* msg) {
			res.set_content(use_json ? subsonic_error_json(code, msg)
			                         : subsonic_error(code, msg),
			                use_json ? "application/json" : "application/xml");
			};
		auto qp = [&](const std::string& k) -> std::string {
			auto it = req.params.find(k); return it != req.params.end() ? it->second : "";
			};
		auto ri = store_.get_user(qp("u"));
		if (!ri || !ri->is_admin) { err(50, "User is not authorized for this operation."); return; }

		// Whether each is set, never what it is.  A secret the server can
		// hand back is a secret in an API response, in the browser's memory
		// and in whatever cache sits between - and, because the web client
		// filled a masked box with it, one the browser's own password manager
		// offered to store as a login.  Nothing needs to read these back: they
		// are written once and used server-side, so the client only has to
		// know whether to say "(set)" or "(not set)".
		//
		// saveServerSettings already writes only the keys the caller sent, so
		// "unchanged" needs no sentinel: the client simply omits the field it
		// did not touch.  Clearing one stays possible by sending it empty.
		bool has_token = !store_.get_setting("discogs_token").empty();
		bool has_tmdb  = !store_.get_setting("tmdb_key").empty();
		std::string body = subsonic_ok_json([&](nlohmann::json& r) {
			r["serverSettings"]["discogsTokenSet"] = has_token;
			r["serverSettings"]["tmdbKeySet"]      = has_tmdb;
			});
		res.set_content(body, "application/json");
		});

	server_.Get("/rest/saveServerSettings.view", [this](const httplib::Request& req,
	                                                     httplib::Response& res) {
		if (!check_auth(req, res, store_)) return;
		bool use_json = (fmt_of(req) == "json");
		auto err = [&](int code, const char* msg) {
			res.set_content(use_json ? subsonic_error_json(code, msg)
			                         : subsonic_error(code, msg),
			                use_json ? "application/json" : "application/xml");
			};
		auto qp = [&](const std::string& k) -> std::string {
			auto it = req.params.find(k); return it != req.params.end() ? it->second : "";
			};
		auto ri = store_.get_user(qp("u"));
		if (!ri || !ri->is_admin) { err(50, "User is not authorized for this operation."); return; }

		// Only settings the caller actually sent are written. With one field
		// that distinction did not exist; with two, a client saving just one
		// of them would otherwise blank the other.
		if (req.params.count("discogsToken"))
			store_.set_setting("discogs_token", qp("discogsToken"));
		if (req.params.count("tmdbKey"))
			store_.set_setting("tmdb_key", qp("tmdbKey"));
		std::string body = use_json ? subsonic_ok_json() : subsonic_ok();
		res.set_content(body, use_json ? "application/json" : "application/xml");
		});

	// getServerStatus, an admin-only live capacity snapshot: worker
	// occupancy, ffmpeg counts, cache pressure, queue depths. JSON only, like
	// getServerSettings: only the web client's Settings pane reads it.
	server_.Get("/rest/getServerStatus.view", [this](const httplib::Request& req,
	                                                  httplib::Response& res) {
		if (!check_auth(req, res, store_)) return;
		bool use_json = (fmt_of(req) == "json");
		auto ui = store_.get_user(req.get_param_value("u"));
		if (!ui || !ui->is_admin) {
			const char* msg = "User is not authorized for this operation.";
			res.set_content(use_json ? subsonic_error_json(50, msg)
			                         : subsonic_error(50, msg),
			                use_json ? "application/json" : "application/xml");
			return;
			}

		auto* pool = http_pool_.load();
		auto  tc   = transcode_cache_.stats();
		auto  cc   = cover_cache_.stats();
		size_t grants = 0;
		{
		std::lock_guard<std::mutex> lock(grant_mu_);
		const auto now = std::chrono::steady_clock::now();
		for (const auto& [tok, g] : grants_)
			if (g.expires > now) grants++;
		}
		size_t fetch_queued = 0;
		{
		std::lock_guard<std::mutex> lock(fetch_mu_);
		fetch_queued = fetch_queue_.size();
		}
		auto scan = store_.scan_status();
		long long uptime = std::chrono::duration_cast<std::chrono::seconds>(
			std::chrono::steady_clock::now() - start_time_).count();
		long long rss = rss_bytes();

		std::string body = subsonic_ok_json([&](nlohmann::json& r) {
			auto& st = r["serverStatus"];
			st["uptimeSeconds"] = uptime;
			if (rss >= 0) st["memoryRssBytes"] = rss;
			// busy includes the worker answering this very request.
			st["http"]["busy"]     = pool ? pool->busy()      : 0;
			st["http"]["queued"]   = pool ? pool->queued()    : 0;
			st["http"]["threads"]  = pool ? pool->threads()   : 0;
			st["http"]["queueCap"] = pool ? pool->queue_cap() : 0;
			st["transcode"]["piped"]         = Streamer::piped_ffmpeg_running();
			st["transcode"]["pipedMax"]      = Streamer::MAX_PIPED_FFMPEG;
			st["transcode"]["running"]       = tc.running;
			st["transcode"]["bgRunning"]     = tc.bg_running;
			st["transcode"]["jobsMax"]       = tc.jobs;
			st["transcode"]["cacheEnabled"]  = tc.enabled;
			st["transcode"]["cacheBytes"]    = tc.used_bytes;
			st["transcode"]["cacheCapBytes"] = tc.cap_bytes;
			st["coverCache"]["memBytes"]     = cc.mem_used;
			st["coverCache"]["memCapBytes"]  = cc.mem_cap;
			st["coverCache"]["entries"]      = cc.entries;
			st["coverCache"]["building"]     = cc.building;
			st["coverCache"]["jobsMax"]      = cc.jobs;
			st["fetch"]["queued"]            = fetch_queued;
			st["fetch"]["queueCap"]          = FETCH_QUEUE_MAX;
			st["scan"]["scanning"]           = scan.scanning;
			st["scan"]["count"]              = scan.count;
			st["cast"]["active"]             = cast_manager_.active();
			st["streamGrants"]  = grants;
			st["loginThrottle"] = throttle_entries();
			});
		res.set_content(body, "application/json");
		});
	}
