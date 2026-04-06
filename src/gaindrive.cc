#include "gaindrive.hh"
#include "stamp.hh"
#include "streamer.hh"

#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <thread>

#include <reproc++/reproc.hpp>

#include <tinyxml2.h>
#include <nlohmann/json.hpp>

using namespace tinyxml2;

// ---- Subsonic XML helpers ---------------------------------------------

static const char* SUBSONIC_NS  = "http://subsonic.org/restapi";
static const char* SUBSONIC_VER = "1.16.1";

// Creates a <subsonic-response> root element inside doc and returns it.
static XMLElement* make_root(XMLDocument& doc, const char* status)
	{
	doc.InsertEndChild(doc.NewDeclaration());
	auto* root = doc.NewElement("subsonic-response");
	root->SetAttribute("xmlns",   SUBSONIC_NS);
	root->SetAttribute("status",  status);
	root->SetAttribute("version", SUBSONIC_VER);
	doc.InsertEndChild(root);
	return root;
	}

static std::string to_string(XMLDocument& doc)
	{
	XMLPrinter printer;
	doc.Print(&printer);
	return printer.CStr();
	}

// Build a complete ok response, optionally populated by a callback.
static std::string subsonic_ok(
	std::function<void(XMLDocument&, XMLElement*)> fn = {})
	{
	XMLDocument doc;
	auto* root = make_root(doc, "ok");
	if (fn) fn(doc, root);
	return to_string(doc);
	}

static std::string subsonic_error(int code, const char* msg)
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

static std::string subsonic_ok_json(
	std::function<void(nlohmann::json&)> fn = {})
	{
	nlohmann::json r;
	r["status"]  = "ok";
	r["version"] = SUBSONIC_VER;
	if (fn) fn(r);
	nlohmann::json j;
	j["subsonic-response"] = r;
	return j.dump();
	}

static std::string subsonic_error_json(int code, const char* msg)
	{
	nlohmann::json j;
	j["subsonic-response"]["status"]           = "failed";
	j["subsonic-response"]["version"]          = SUBSONIC_VER;
	j["subsonic-response"]["error"]["code"]    = code;
	j["subsonic-response"]["error"]["message"] = msg;
	return j.dump();
	}

// Returns "json" if the client requested JSON, otherwise "xml".
static std::string fmt_of(const httplib::Request& req)
	{
	auto it = req.params.find("f");
	return (it != req.params.end() && it->second == "json") ? "json" : "xml";
	}

// ---- Helpers ----------------------------------------------------------

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

static const char* codec_to_mime(const std::string& codec)
	{
	if (codec == "flac")            return "audio/flac";
	if (codec == "mp3")             return "audio/mpeg";
	if (codec == "ogg")             return "audio/ogg";
	if (codec == "opus")            return "audio/ogg";
	if (codec == "m4a")             return "audio/mp4";
	if (codec == "aac")             return "audio/aac";
	if (codec == "wav")             return "audio/wav";
	if (codec == "wma")             return "audio/x-ms-wma";
	return "application/octet-stream";
	}

static std::string url_encode(const std::string& s)
	{
	static const char hex[] = "0123456789ABCDEF";
	std::string out;
	for (unsigned char c : s) {
		if (std::isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~')
			out += c;
		else { out += '%'; out += hex[c >> 4]; out += hex[c & 0xf]; }
		}
	return out;
	}

// Scales an image file to fit within size×size pixels (JPEG output) using
// ffmpeg.  Isolated here so it can be swapped for a proper image library later.
static void serve_cover_scaled(httplib::Response& res,
                                const std::string& path, int size)
	{
	std::vector<std::string> args = {
		"ffmpeg", "-i", path,
		"-vf", "scale=" + std::to_string(size) + ":" + std::to_string(size)
		       + ":force_original_aspect_ratio=decrease",
		"-frames:v", "1", "-f", "mjpeg", "pipe:1"
		};

	auto proc = std::make_shared<reproc::process>();
	reproc::options opts;
	opts.redirect.err.type = reproc::redirect::type::discard;

	auto ec = proc->start(args, opts);
	if (ec) {
		std::cout << stamp() << "getCoverArt: ffmpeg launch failed: "
		          << ec.message() << std::endl;
		res.status = 500;
		return;
		}

	res.set_content_provider(
		"image/jpeg",
		[proc](size_t, httplib::DataSink& sink) {
			uint8_t buf[65536];
			auto [n, err] = proc->read(reproc::stream::out, buf, sizeof(buf));
			if (n == 0) { sink.done(); return false; }
			return sink.write(reinterpret_cast<char*>(buf), n);
			},
		[proc](bool success) {
			if (!success) proc->terminate();
			proc->wait(reproc::infinite);
			});
	}

// Extracts u/p/t/s params and validates auth. Writes error into res on failure.
static bool check_auth(const httplib::Request& req, httplib::Response& res,
                       MediaStore& store)
	{
	auto qp = [&](const std::string& k) -> std::string {
		auto it = req.params.find(k);
		return it != req.params.end() ? it->second : "";
		};

	std::string u  = qp("u");
	std::string pw = qp("p");
	std::string t  = qp("t");
	std::string s  = qp("s");

	bool use_json = (fmt_of(req) == "json");
	auto err = [&](int code, const char* msg) {
		if (use_json)
			res.set_content(subsonic_error_json(code, msg), "application/json");
		else
			res.set_content(subsonic_error(code, msg),      "application/xml");
		};

	if (u.empty() || (pw.empty() && (t.empty() || s.empty()))) {
		err(10, "Required parameter missing.");
		return false;
		}

	if (!store.validate_auth(u, pw, t, s)) {
		err(40, "Wrong username or password.");
		return false;
		}

	return true;
	}

// ---- GainDrive --------------------------------------------------------

GainDrive::GainDrive(const std::string& db_path,
                     const std::string& music_root,
                     bool no_scan,
                     bool debug)
	: debug_(debug), store_(db_path, music_root)
	{
	server_.set_logger([](const httplib::Request& req, const httplib::Response& res) {
		std::cout << stamp(req.remote_addr)
		          << req.method << " " << req.path;
		if (!req.params.empty()) {
			std::cout << "?";
			bool first = true;
			for (auto& [k, v] : req.params) {
				if (!first) std::cout << "&";
				std::cout << k << "=" << v;
				first = false;
				}
			}
		std::cout << " -> " << res.status << std::endl;
		});

	// ping
	server_.Get("/rest/ping.view", [this](const httplib::Request& req,
	                                      httplib::Response& res) {
		if (!check_auth(req, res, store_)) return;
		bool use_json = (fmt_of(req) == "json");
		std::string body = use_json ? subsonic_ok_json() : subsonic_ok();
		if (debug_) std::cout << body << "\n";
		res.set_content(body, use_json ? "application/json" : "application/xml");
		});

	// getLicense — perpetually-valid dummy.
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
		if (debug_) std::cout << body << "\n";
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
					{"uploadRole",        false},
					{"playlistRole",      true},
					{"coverArtRole",      true},
					{"commentRole",       false},
					{"podcastRole",       false},
					{"streamRole",        true},
					{"jukeboxRole",       false},
					{"shareRole",         false}
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
				u->SetAttribute("uploadRole",        false);
				u->SetAttribute("playlistRole",      true);
				u->SetAttribute("coverArtRole",      true);
				u->SetAttribute("commentRole",       false);
				u->SetAttribute("podcastRole",       false);
				u->SetAttribute("streamRole",        true);
				u->SetAttribute("jukeboxRole",       false);
				u->SetAttribute("shareRole",         false);
				root->InsertEndChild(u);
				});
		if (debug_) std::cout << body << "\n";
		res.set_content(body, use_json ? "application/json" : "application/xml");
		});

	// getMusicFolders — returns the configured music root(s).
	server_.Get("/rest/getMusicFolders.view", [this](const httplib::Request& req,
	                                                  httplib::Response& res) {
		if (!check_auth(req, res, store_)) return;
		auto folders = store_.get_music_folders();
		bool use_json = (fmt_of(req) == "json");
		std::string body;
		if (use_json)
			body = subsonic_ok_json([&folders](nlohmann::json& r) {
				nlohmann::json arr = nlohmann::json::array();
				for (auto& f : folders)
					arr.push_back({{"id", f.id}, {"name", f.name}});
				r["musicFolders"]["musicFolder"] = arr;
				});
		else
			body = subsonic_ok([&folders](XMLDocument& doc, XMLElement* root) {
				auto* mf = doc.NewElement("musicFolders");
				for (auto& f : folders) {
					auto* el = doc.NewElement("musicFolder");
					el->SetAttribute("id",   f.id);
					el->SetAttribute("name", f.name.c_str());
					mf->InsertEndChild(el);
					}
				root->InsertEndChild(mf);
				});
		if (debug_) std::cout << body << "\n";
		res.set_content(body, use_json ? "application/json" : "application/xml");
		});

	// getIndexes — all artists grouped by first letter.
	server_.Get("/rest/getIndexes.view", [this](const httplib::Request& req,
	                                             httplib::Response& res) {
		if (!check_auth(req, res, store_)) return;

		auto artists = store_.get_artist_dirs();

		std::sort(artists.begin(), artists.end(),
			[](const MediaStore::ArtistDir& a, const MediaStore::ArtistDir& b) {
				return sort_key(a.name) < sort_key(b.name);
				});

		// Build letter → artists map (shared by both branches).
		std::map<std::string, std::vector<const MediaStore::ArtistDir*>> buckets;
		for (auto& a : artists) {
			std::string key    = sort_key(a.name);
			std::string letter = key.empty() || !std::isalpha((unsigned char)key[0])
			                   ? "#"
			                   : std::string(1, (char)std::toupper((unsigned char)key[0]));
			buckets[letter].push_back(&a);
			}

		bool use_json = (fmt_of(req) == "json");
		std::string body;
		if (use_json)
			body = subsonic_ok_json([&buckets](nlohmann::json& r) {
				nlohmann::json idx_arr = nlohmann::json::array();
				for (auto& [letter, vec] : buckets) {
					nlohmann::json artist_arr = nlohmann::json::array();
					for (auto* a : vec)
						artist_arr.push_back({{"id", a->id}, {"name", a->name}});
					idx_arr.push_back({{"name", letter}, {"artist", artist_arr}});
					}
				r["indexes"] = {
					{"lastModified",    "0"},
					{"ignoredArticles", "The El La Los Las Le Les A An Die Das Ein Eine"},
					{"index",           idx_arr}
					};
				});
		else
			body = subsonic_ok([&buckets](XMLDocument& doc, XMLElement* root) {
				auto* indexes = doc.NewElement("indexes");
				indexes->SetAttribute("lastModified",    "0");
				indexes->SetAttribute("ignoredArticles",
					"The El La Los Las Le Les A An Die Das Ein Eine");
				for (auto& [letter, vec] : buckets) {
					auto* idx = doc.NewElement("index");
					idx->SetAttribute("name", letter.c_str());
					for (auto* a : vec) {
						auto* artist = doc.NewElement("artist");
						artist->SetAttribute("id",   a->id);
						artist->SetAttribute("name", a->name.c_str());
						idx->InsertEndChild(artist);
						}
					indexes->InsertEndChild(idx);
					}
				root->InsertEndChild(indexes);
				});
		if (debug_) std::cout << body << "\n";
		res.set_content(body, use_json ? "application/json" : "application/xml");
		});

	// getMusicDirectory — contents of a folder (album dirs or song files).
	server_.Get("/rest/getMusicDirectory.view", [this](const httplib::Request& req,
	                                                    httplib::Response& res) {
		if (!check_auth(req, res, store_)) return;

		bool use_json = (fmt_of(req) == "json");
		auto err = [&](int code, const char* msg) {
			if (use_json)
				res.set_content(subsonic_error_json(code, msg), "application/json");
			else
				res.set_content(subsonic_error(code, msg),      "application/xml");
			};

		auto it = req.params.find("id");
		if (it == req.params.end()) { err(10, "Required parameter missing: id."); return; }

		auto dir = store_.get_directory(std::stoi(it->second));
		if (!dir) { err(70, "Directory not found."); return; }

		std::string body;
		if (use_json)
			body = subsonic_ok_json([&dir](nlohmann::json& r) {
				nlohmann::json children = nlohmann::json::array();
				for (auto& c : dir->children) {
					nlohmann::json child = {
						{"id",     c.id},
						{"parent", c.parent_id},
						{"isDir",  c.is_dir},
						{"title",  c.title},
						{"artist", c.artist},
						{"album",  c.album}
						};
					if (c.cover_art_id >= 0) child["coverArt"] = c.cover_art_id;
					if (!c.is_dir) {
						child["track"]       = c.track_number;
						child["discNumber"]  = c.disc_number;
						child["year"]        = c.year;
						child["genre"]       = c.genre;
						child["size"]        = c.file_size;
						child["contentType"] = codec_to_mime(c.codec);
						child["suffix"]      = c.codec;
						child["duration"]    = (int)c.duration;
						child["bitRate"]     = c.bitrate;
						child["path"]        = c.path;
						}
					children.push_back(child);
					}
				r["directory"] = {
					{"id",    dir->id},
					{"name",  dir->name},
					{"child", children}
					};
				if (dir->parent_id >= 0)    r["directory"]["parent"]   = dir->parent_id;
				if (dir->cover_art_id >= 0) r["directory"]["coverArt"] = dir->cover_art_id;
				});
		else
			body = subsonic_ok([&dir](XMLDocument& doc, XMLElement* root) {
				auto* directory = doc.NewElement("directory");
				directory->SetAttribute("id",   dir->id);
				directory->SetAttribute("name", dir->name.c_str());
				if (dir->parent_id >= 0)
					directory->SetAttribute("parent", dir->parent_id);
				if (dir->cover_art_id >= 0)
					directory->SetAttribute("coverArt", dir->cover_art_id);

				for (auto& c : dir->children) {
					auto* child = doc.NewElement("child");
					child->SetAttribute("id",     c.id);
					child->SetAttribute("parent", c.parent_id);
					child->SetAttribute("isDir",  c.is_dir);
					child->SetAttribute("title",  c.title.c_str());
					child->SetAttribute("artist", c.artist.c_str());
					child->SetAttribute("album",  c.album.c_str());
					if (c.cover_art_id >= 0)
						child->SetAttribute("coverArt", c.cover_art_id);
					if (!c.is_dir) {
						child->SetAttribute("track",       c.track_number);
						child->SetAttribute("discNumber",  c.disc_number);
						child->SetAttribute("year",        c.year);
						child->SetAttribute("genre",       c.genre.c_str());
						child->SetAttribute("size",        (int64_t)c.file_size);
						child->SetAttribute("contentType", codec_to_mime(c.codec));
						child->SetAttribute("suffix",      c.codec.c_str());
						child->SetAttribute("duration",    (int)c.duration);
						child->SetAttribute("bitRate",     c.bitrate);
						child->SetAttribute("path",        c.path.c_str());
						}
					directory->InsertEndChild(child);
					}

				root->InsertEndChild(directory);
				});
		if (debug_) std::cout << body << "\n";
		res.set_content(body, use_json ? "application/json" : "application/xml");
		});

	// getArtistInfo — MusicBrainz lookup, result cached in DB.
	server_.Get("/rest/getArtistInfo.view", [this](const httplib::Request& req,
	                                               httplib::Response& res) {
		if (!check_auth(req, res, store_)) return;

		bool use_json = (fmt_of(req) == "json");
		auto err = [&](int code, const char* msg) {
			if (use_json)
				res.set_content(subsonic_error_json(code, msg), "application/json");
			else
				res.set_content(subsonic_error(code, msg),      "application/xml");
			};

		auto it = req.params.find("id");
		if (it == req.params.end()) { err(10, "Required parameter missing: id."); return; }

		int id = std::stoi(it->second);
		std::string name = store_.get_folder_name(id);
		if (name.empty()) { err(70, "Artist not found."); return; }

		auto cached = store_.get_cached_artist_info(id);
		MediaStore::CachedArtistInfo info;
		if (cached) {
			info = *cached;
			std::cout << stamp() << "getArtistInfo [" << name << "] cached"
			          << " mbid=" << (info.mbid.empty() ? "(none)" : info.mbid)
			          << std::endl;
			}
		else {
			// Query MusicBrainz; cache result (even if empty) to avoid repeat lookups.
			std::cout << stamp() << "getArtistInfo [" << name << "] querying MusicBrainz"
			          << std::endl;
			httplib::SSLClient mb("musicbrainz.org");
			mb.set_default_headers({
				{"User-Agent", "GainDrive/0.1 (https://github.com/kpeeters/gaindrive)"}
				});
			httplib::Params params{
				{"query", "artist:\"" + name + "\""},
				{"limit", "1"},
				{"fmt",   "json"}
				};
			auto r = mb.Get("/ws/2/artist", params, httplib::Headers{});
			if (!r) {
				std::cout << stamp() << "getArtistInfo [" << name
				          << "] MusicBrainz request failed (no response)" << std::endl;
				}
			else if (r->status != 200) {
				std::cout << stamp() << "getArtistInfo [" << name
				          << "] MusicBrainz HTTP " << r->status << std::endl;
				}
			else {
				auto j = nlohmann::json::parse(r->body, nullptr, false);
				if (!j.is_discarded() && j.contains("artists") && !j["artists"].empty()) {
					info.mbid = j["artists"][0].value("id", "");
					if (!info.mbid.empty())
						info.last_fm_url = "https://www.last.fm/music/" + url_encode(name);
					}
				}

			// Step 2 — MusicBrainz URL relations → Wikipedia article URL.
			if (!info.mbid.empty()) {
				std::this_thread::sleep_for(std::chrono::seconds(1));
				httplib::Params p2{{"inc","url-rels"},{"fmt","json"}};
				auto r2 = mb.Get("/ws/2/artist/" + info.mbid, p2, httplib::Headers{});
				if (!r2) {
					std::cout << stamp() << "getArtistInfo [" << name
					          << "] MusicBrainz url-rels request failed" << std::endl;
					}
				else if (r2->status != 200) {
					std::cout << stamp() << "getArtistInfo [" << name
					          << "] MusicBrainz url-rels HTTP " << r2->status << std::endl;
					}
				else {
					auto j2 = nlohmann::json::parse(r2->body, nullptr, false);
					auto rels = j2.value("relations", nlohmann::json::array());
					std::cout << stamp() << "getArtistInfo [" << name
					          << "] MusicBrainz url-rels: " << rels.size() << " relation(s)";
					for (auto& rel : rels)
						std::cout << " [" << rel.value("type","?") << "]";
					std::cout << std::endl;

					for (auto& rel : rels) {
						if (rel.value("type","") != "wikipedia") continue;
						std::string wiki_url = rel.value("url", nlohmann::json::object())
						                           .value("resource","");
						auto pos = wiki_url.find("/wiki/");
						if (pos == std::string::npos) continue;
						std::string title = wiki_url.substr(pos + 6);
						std::cout << stamp() << "getArtistInfo [" << name
						          << "] Wikipedia title: " << title << std::endl;
						// Step 3 — Wikipedia REST summary → bio + thumbnail.
						httplib::SSLClient wp("en.wikipedia.org");
						wp.set_default_headers({
							{"User-Agent","GainDrive/0.1 (https://github.com/kpeeters/gaindrive)"}
							});
						auto r3 = wp.Get("/api/rest_v1/page/summary/" + title,
						                 httplib::Params{}, httplib::Headers{});
						if (!r3) {
							std::cout << stamp() << "getArtistInfo [" << name
							          << "] Wikipedia request failed" << std::endl;
							}
						else if (r3->status != 200) {
							std::cout << stamp() << "getArtistInfo [" << name
							          << "] Wikipedia HTTP " << r3->status << std::endl;
							}
						else {
							auto j3 = nlohmann::json::parse(r3->body, nullptr, false);
							if (!j3.is_discarded()) {
								info.biography = j3.value("extract","");
								if (j3.contains("thumbnail"))
									info.image_url = j3["thumbnail"].value("source","");
								std::cout << stamp() << "getArtistInfo [" << name
								          << "] bio=" << info.biography.size()
								          << " chars, image="
								          << (info.image_url.empty() ? "(none)" : info.image_url)
								          << std::endl;
								}
							}
						break;
						}
					}
				}

			store_.cache_artist_info(id, info);
			std::cout << stamp() << "getArtistInfo [" << name << "] cached"
			          << " mbid=" << (info.mbid.empty() ? "(none)" : info.mbid)
			          << std::endl;
			}

		std::string body;
		if (use_json)
			body = subsonic_ok_json([&info](nlohmann::json& r) {
				nlohmann::json ai = nlohmann::json::object();
				if (!info.biography.empty())   ai["biography"]      = info.biography;
				if (!info.mbid.empty())        ai["musicBrainzId"]  = info.mbid;
				if (!info.last_fm_url.empty()) ai["lastFmUrl"]      = info.last_fm_url;
				if (!info.image_url.empty()) {
					ai["smallImageUrl"]  = info.image_url;
					ai["mediumImageUrl"] = info.image_url;
					ai["largeImageUrl"]  = info.image_url;
					}
				r["artistInfo"] = ai;
				});
		else
			body = subsonic_ok([&info](XMLDocument& doc, XMLElement* root) {
				auto* ai = doc.NewElement("artistInfo");
				if (!info.biography.empty()) {
					auto* bio = doc.NewElement("biography");
					bio->SetText(info.biography.c_str());
					ai->InsertEndChild(bio);
					}
				if (!info.mbid.empty())
					ai->SetAttribute("musicBrainzId", info.mbid.c_str());
				if (!info.last_fm_url.empty())
					ai->SetAttribute("lastFmUrl", info.last_fm_url.c_str());
				if (!info.image_url.empty()) {
					ai->SetAttribute("smallImageUrl",  info.image_url.c_str());
					ai->SetAttribute("mediumImageUrl", info.image_url.c_str());
					ai->SetAttribute("largeImageUrl",  info.image_url.c_str());
					}
				root->InsertEndChild(ai);
				});
		if (debug_) std::cout << body << "\n";
		res.set_content(body, use_json ? "application/json" : "application/xml");
		});

	// getCoverArt — serve a cover image, optionally scaled.
	server_.Get("/rest/getCoverArt.view", [this](const httplib::Request& req,
	                                              httplib::Response& res) {
		if (!check_auth(req, res, store_)) return;

		auto it = req.params.find("id");
		if (it == req.params.end()) {
			res.set_content(subsonic_error(10, "Required parameter missing: id."),
			                "application/xml");
			return;
			}

		std::string path = store_.get_cover_path(std::stoi(it->second));
		if (path.empty()) {
			res.status = 404;
			return;
			}

		auto size_it = req.params.find("size");
		if (size_it != req.params.end()) {
			serve_cover_scaled(res, path, std::stoi(size_it->second));
			return;
			}

		// Serve the full-size image directly.
		namespace fs = std::filesystem;
		auto file_size = static_cast<size_t>(fs::file_size(path));
		res.set_content_provider(
			file_size, "image/jpeg",
			[path](size_t offset, size_t length, httplib::DataSink& sink) {
				std::ifstream f(path, std::ios::binary);
				if (!f) return false;
				f.seekg(static_cast<std::streamoff>(offset));
				char   buf[65536];
				size_t remaining = length;
				while (remaining > 0) {
					auto to_read = static_cast<std::streamsize>(
					    std::min(remaining, sizeof(buf)));
					f.read(buf, to_read);
					auto n = static_cast<size_t>(f.gcount());
					if (n == 0) break;
					if (!sink.write(buf, n)) return false;
					remaining -= n;
					}
				return true;
				});
		});

	// savePlayQueue — persist the client's current queue and playback position.
	server_.Get("/rest/savePlayQueue.view", [this](const httplib::Request& req,
	                                               httplib::Response& res) {
		if (!check_auth(req, res, store_)) return;

		auto qp = [&](const std::string& k, const std::string& def = "") {
			auto it = req.params.find(k);
			return it != req.params.end() ? it->second : def;
			};

		// Collect all song IDs — the parameter may be repeated.
		std::vector<int> ids;
		auto range = req.params.equal_range("id");
		for (auto it = range.first; it != range.second; ++it)
			ids.push_back(std::stoi(it->second));

		int     current_id = ids.empty() ? 0 : std::stoi(qp("current", "0"));
		int64_t offset_ms  = std::stoll(qp("position", "0"));
		std::string client = qp("c");
		std::string user   = qp("u");

		store_.save_play_queue(user, ids, current_id, offset_ms, client);
		bool use_json = (fmt_of(req) == "json");
		std::string body = use_json ? subsonic_ok_json() : subsonic_ok();
		if (debug_) std::cout << body << "\n";
		res.set_content(body, use_json ? "application/json" : "application/xml");
		});

	// createBookmark — mark a playback position within a song.
	server_.Get("/rest/createBookmark.view", [this](const httplib::Request& req,
	                                                httplib::Response& res) {
		if (!check_auth(req, res, store_)) return;

		auto qp = [&](const std::string& k, const std::string& def = "") {
			auto it = req.params.find(k);
			return it != req.params.end() ? it->second : def;
			};

		auto it = req.params.find("id");
		if (it == req.params.end()) {
			res.set_content(subsonic_error(10, "Required parameter missing: id."),
			                "application/xml");
			return;
			}

		int     song_id     = std::stoi(it->second);
		int64_t position_ms = std::stoll(qp("position", "0"));
		std::string comment = qp("comment");
		std::string user    = qp("u");

		store_.create_bookmark(user, song_id, position_ms, comment);
		bool use_json = (fmt_of(req) == "json");
		std::string body = use_json ? subsonic_ok_json() : subsonic_ok();
		if (debug_) std::cout << body << "\n";
		res.set_content(body, use_json ? "application/json" : "application/xml");
		});

	// stream — serve audio file directly or transcode via ffmpeg.
	server_.Get("/rest/stream.view", [this](const httplib::Request& req,
	                                        httplib::Response& res) {
		if (!check_auth(req, res, store_)) return;

		auto it = req.params.find("id");
		if (it == req.params.end()) {
			res.set_content(subsonic_error(10, "Required parameter missing: id."),
			                "application/xml");
			return;
			}

		auto song = store_.get_song(std::stoi(it->second));
		if (!song) {
			res.set_content(subsonic_error(70, "Song not found."), "application/xml");
			return;
			}

		auto qp = [&](const std::string& k, const std::string& def = "") {
			auto it2 = req.params.find(k);
			return it2 != req.params.end() ? it2->second : def;
			};

		int         max_bitrate = std::stoi(qp("maxBitRate", "0"));
		std::string format      = qp("format");
		int         time_offset = std::stoi(qp("timeOffset", "0"));

		Streamer::SongInfo si{ song->path, song->codec, song->bitrate,
		                       song->duration, song->file_size };
		Streamer::serve(req, res, si, max_bitrate, format, time_offset);
		});

	// Catch-all for endpoints not yet implemented.
	server_.Get("/rest/:endpoint", [](const httplib::Request& req, httplib::Response& res) {
		std::cout << stamp() << "NOT IMPLEMENTED: " << req.path << std::endl;
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
