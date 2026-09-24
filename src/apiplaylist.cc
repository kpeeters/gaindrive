#include "gaindrive.hh"
#include "subsonic.hh"
#include "authz.hh"
#include "stamp.hh"
#include "apientry.hh"
#include "textutil.hh"
#include "untrusted.hh"

#include <iostream>

#include <tinyxml2.h>
#include <nlohmann/json.hpp>

using namespace tinyxml2;

void GainDrive::routes_playlist()
	{
	// savePlayQueue - persist the client's current queue and playback position.

	server_.Get("/rest/savePlayQueue.view", [this](const httplib::Request& req,
	                                               httplib::Response& res) {
		if (!check_auth(req, res, store_)) return;

		auto qp = [&](const std::string& k, const std::string& def = "") {
			auto it = req.params.find(k);
			return it != req.params.end() ? it->second : def;
			};

		// Collect all song IDs - the parameter may be repeated. Resolve
		// each to its filesystem path (the durable key the user-state DB
		// stores). Unresolvable ids are dropped silently.
		std::vector<std::string> paths;
		auto range = req.params.equal_range("id");
		for (auto it = range.first; it != range.second; ++it) {
			if (auto p = store_.song_path_by_id(to_int(it->second, -1)))
				if (item_read_allowed(req, store_, uploads_root_name_, *p))
					paths.push_back(*p);
			}

		int         current_id   = to_int(qp("current", "0"), 0);
		std::string current_path = "";
		if (current_id != 0) {
			if (auto p = store_.song_path_by_id(current_id))
				if (item_read_allowed(req, store_, uploads_root_name_, *p))
					current_path = *p;
			}
		int64_t offset_ms  = to_int64(qp("position", "0"), 0);
		std::string client = qp("c");
		std::string user   = qp("u");

		store_.save_play_queue(user, paths, current_path, offset_ms, client);
		bool use_json = (fmt_of(req) == "json");
		std::string body = use_json ? subsonic_ok_json() : subsonic_ok();
		res.set_content(body, use_json ? "application/json" : "application/xml");
		});

	// getPlayQueue - retrieve the user's saved play queue and position.
	server_.Get("/rest/getPlayQueue.view", [this](const httplib::Request& req,
	                                              httplib::Response& res) {
		if (!check_auth(req, res, store_)) return;
		bool use_json = (fmt_of(req) == "json");
		std::string user = req.params.find("u")->second;
		auto pq = store_.get_play_queue(user);
		int mbr = request_max_bitrate(req, store_);

		std::string body;
		if (use_json)
			body = subsonic_ok_json([&pq, &user, mbr](nlohmann::json& r) {
				if (!pq) { r["playQueue"] = nlohmann::json::object(); return; }
				nlohmann::json entries = nlohmann::json::array();
				for (auto& s : pq->songs)
					entries.push_back(song_entry_json(s, mbr));
				r["playQueue"] = {
					{"current",    pq->current_id},
					{"position",   pq->offset_ms},
					{"username",   user},
					{"changed",    iso8601(pq->changed)},
					{"changedBy",  pq->client},
					{"entry",      entries}
					};
				});
		else
			body = subsonic_ok([&pq, &user, mbr](XMLDocument& doc, XMLElement* root) {
				auto* el = doc.NewElement("playQueue");
				if (pq) {
					el->SetAttribute("current",   pq->current_id);
					el->SetAttribute("position",  (int64_t)pq->offset_ms);
					el->SetAttribute("username",  user.c_str());
					if (!pq->changed.empty())
						el->SetAttribute("changed",   iso8601(pq->changed).c_str());
					if (!pq->client.empty())
						el->SetAttribute("changedBy", pq->client.c_str());
					for (auto& s : pq->songs)
						el->InsertEndChild(song_entry_xml(doc, s, "entry", mbr));
					}
				root->InsertEndChild(el);
				});
		res.set_content(body, use_json ? "application/json" : "application/xml");
		});

	// createBookmark - mark a playback position within a song.
	// scrobble - record a play (submission=true) or now-playing event (submission=false).
	server_.Get("/rest/scrobble.view", [this](const httplib::Request& req,
	                                          httplib::Response& res) {
		if (!check_auth(req, res, store_)) return;

		auto qp = [&](const std::string& k, const std::string& def = "") {
			auto it = req.params.find(k);
			return it != req.params.end() ? it->second : def;
			};

		std::string user = qp("u");
		std::string client = qp("c");
		// submission defaults to true per the Subsonic spec.
		bool submission = (qp("submission", "true") != "false");

		// The read-permission filter on every id: these client-state writes
		// come back out through getStarred/getPlaylist/getPlayQueue with the
		// full song entry, so an id resolved here without the check is a
		// metadata read of somebody else's uploads by integer-guessing.
		// Foreign ids are dropped like unresolvable ones rather than failing
		// the request, matching how a deleted id already behaves.
		auto range = req.params.equal_range("id");
		for (auto it = range.first; it != range.second; ++it) {
			if (auto p = store_.song_path_by_id(to_int(it->second, -1)))
				if (item_read_allowed(req, store_, uploads_root_name_, *p))
					store_.scrobble(user, *p, submission, client);
			}

		bool use_json = (fmt_of(req) == "json");
		std::string body = use_json ? subsonic_ok_json() : subsonic_ok();
		res.set_content(body, use_json ? "application/json" : "application/xml");
		});

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

		int     song_id     = to_int(it->second, -1);
		int64_t position_ms = to_int64(qp("position", "0"), 0);
		std::string comment = qp("comment");
		std::string user    = qp("u");

		auto song_path = store_.song_path_by_id(song_id);
		if (!song_path
		    || !item_read_allowed(req, store_, uploads_root_name_, *song_path)) {
			res.set_content(subsonic_error(70, "Song not found."), "application/xml");
			return;
			}
		store_.create_bookmark(user, *song_path, position_ms, comment);
		bool use_json = (fmt_of(req) == "json");
		std::string body = use_json ? subsonic_ok_json() : subsonic_ok();
		res.set_content(body, use_json ? "application/json" : "application/xml");
		});
	// createPlaylist

	server_.Get("/rest/createPlaylist.view", [this](const httplib::Request& req,
	                                                 httplib::Response& res) {
		if (!check_auth(req, res, store_)) return;
		bool use_json = (fmt_of(req) == "json");

		auto err = [&](int code, const char* msg) {
			if (use_json)
				res.set_content(subsonic_error_json(code, msg), "application/json");
			else
				res.set_content(subsonic_error(code, msg),      "application/xml");
			};

		auto it = req.params.find("name");
		// clean_name for the same reason updateSong's title gets it: this is
		// typed text on its way into a column both response formats serve.
		std::string name = it != req.params.end() ? clean_name(it->second)
		                                          : std::string();
		if (name.empty()) {
			err(10, "Required parameter missing: name.");
			return;
			}
		std::string user = req.params.find("u")->second;

		std::vector<std::string> song_paths;
		auto range = req.params.equal_range("songId");
		for (auto i = range.first; i != range.second; ++i) {
			if (auto p = store_.song_path_by_id(to_int(i->second, -1)))
				if (item_read_allowed(req, store_, uploads_root_name_, *p))
					song_paths.push_back(*p);
			}

		auto pl = store_.create_playlist(user, name, song_paths);
		std::string body = playlist_body(pl, use_json, request_max_bitrate(req, store_));
		res.set_content(body, use_json ? "application/json" : "application/xml");
		});

	// getPlaylist
	server_.Get("/rest/getPlaylist.view", [this](const httplib::Request& req,
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

		std::string user = req.params.find("u")->second;
		// One message for "no such playlist" and "not yours", the same
		// reasoning as deleteUpload's: the difference is only useful to
		// somebody walking the id space.
		auto pl = store_.get_playlist(to_int(it->second, -1), user);
		if (!pl) { err(70, "Playlist not found."); return; }

		std::string body = playlist_body(*pl, use_json, request_max_bitrate(req, store_));
		res.set_content(body, use_json ? "application/json" : "application/xml");
		});

	// getPlaylists
	server_.Get("/rest/getPlaylists.view", [this](const httplib::Request& req,
	                                               httplib::Response& res) {
		if (!check_auth(req, res, store_)) return;
		bool use_json = (fmt_of(req) == "json");
		std::string user = req.params.find("u")->second;
		auto pls = store_.get_playlists(user);

		std::string body;
		if (use_json)
			body = subsonic_ok_json([&pls](nlohmann::json& r) {
				nlohmann::json arr = nlohmann::json::array();
				for (auto& pl : pls)
					arr.push_back({
						{"id",        sid(pl.id)},
						{"name",      pl.name},
						{"comment",   pl.comment},
						{"owner",     pl.owner},
						{"public",    pl.is_public},
						{"songCount", pl.song_count},
						{"duration",  pl.duration},
						{"created",   iso8601(pl.created)},
						{"changed",   iso8601(pl.updated)}
						});
				r["playlists"] = {{"playlist", arr}};
				});
		else
			body = subsonic_ok([&pls](XMLDocument& doc, XMLElement* root) {
				auto* playlists = doc.NewElement("playlists");
				for (auto& pl : pls) {
					auto* el = doc.NewElement("playlist");
					el->SetAttribute("id",        pl.id);
					el->SetAttribute("name",      pl.name.c_str());
					el->SetAttribute("comment",   pl.comment.c_str());
					el->SetAttribute("owner",     pl.owner.c_str());
					el->SetAttribute("public",    pl.is_public);
					el->SetAttribute("songCount", pl.song_count);
					el->SetAttribute("duration",  pl.duration);
					el->SetAttribute("created",   iso8601(pl.created).c_str());
					el->SetAttribute("changed",   iso8601(pl.updated).c_str());
					playlists->InsertEndChild(el);
					}
				root->InsertEndChild(playlists);
				});
		res.set_content(body, use_json ? "application/json" : "application/xml");
		});

	// getStarred / getStarred2 - identical content, only the response key differs.
	auto starred_handler = [this](const httplib::Request& req, httplib::Response& res,
	                               const char* key) {
		if (!check_auth(req, res, store_)) return;
		bool use_json = (fmt_of(req) == "json");
		std::string user = req.params.find("u")->second;
		auto sr = store_.get_starred(user);
		int mbr = request_max_bitrate(req, store_);

		std::string body;
		if (use_json)
			body = subsonic_ok_json([&sr, key, mbr](nlohmann::json& r) {
				nlohmann::json artists = nlohmann::json::array();
				for (auto& a : sr.artists)
					artists.push_back({{"id", sid(a.id)}, {"name", a.name}});

				nlohmann::json albums = nlohmann::json::array();
				for (auto& c : sr.albums) {
					nlohmann::json al = {
						{"id",     sid(c.id)},
						{"parent", sid(c.parent_id)},
						{"isDir",  true},
						{"title",  c.title},
						{"artist", c.artist},
						{"album",  c.album}
						};
					if (c.cover_art_id >= 0) al["coverArt"] = sid(c.cover_art_id);
					albums.push_back(al);
					}

				nlohmann::json songs = nlohmann::json::array();
				for (auto& c : sr.songs)
					songs.push_back(song_entry_json(c, mbr));

				r[key] = {
					{"artist", artists},
					{"album",  albums},
					{"song",   songs}
					};
				});
		else
			body = subsonic_ok([&sr, key, mbr](XMLDocument& doc, XMLElement* root) {
				auto* starred = doc.NewElement(key);

				for (auto& a : sr.artists) {
					auto* el = doc.NewElement("artist");
					el->SetAttribute("id",   a.id);
					el->SetAttribute("name", a.name.c_str());
					starred->InsertEndChild(el);
					}

				for (auto& c : sr.albums) {
					auto* el = doc.NewElement("album");
					el->SetAttribute("id",     c.id);
					el->SetAttribute("parent", c.parent_id);
					el->SetAttribute("isDir",  true);
					el->SetAttribute("title",  c.title.c_str());
					el->SetAttribute("artist", c.artist.c_str());
					el->SetAttribute("album",  c.album.c_str());
					if (c.cover_art_id >= 0) el->SetAttribute("coverArt", c.cover_art_id);
					starred->InsertEndChild(el);
					}

				for (auto& c : sr.songs)
					starred->InsertEndChild(song_entry_xml(doc, c, "song", mbr));

				root->InsertEndChild(starred);
				});
		res.set_content(body, use_json ? "application/json" : "application/xml");
		};
	server_.Get("/rest/getStarred.view",  [starred_handler](const httplib::Request& req,
	                                                         httplib::Response& res) {
		starred_handler(req, res, "starred");
		});
	server_.Get("/rest/getStarred2.view", [starred_handler](const httplib::Request& req,
	                                                          httplib::Response& res) {
		starred_handler(req, res, "starred2");
		});

	// updatePlaylist
	server_.Get("/rest/updatePlaylist.view", [this](const httplib::Request& req,
	                                                 httplib::Response& res) {
		if (!check_auth(req, res, store_)) return;
		bool use_json = (fmt_of(req) == "json");

		auto err = [&](int code, const char* msg) {
			if (use_json)
				res.set_content(subsonic_error_json(code, msg), "application/json");
			else
				res.set_content(subsonic_error(code, msg),      "application/xml");
			};

		auto it = req.params.find("playlistId");
		if (it == req.params.end()) { err(10, "Required parameter missing: playlistId."); return; }
		int playlist_id = to_int(it->second, -1);
		std::string user = req.params.find("u")->second;

		std::optional<std::string> name, comment;
		std::optional<bool> is_public;
		if (req.params.count("name"))    name      = clean_name(req.params.find("name")->second);
		if (req.params.count("comment")) comment   = clean_prose(req.params.find("comment")->second,
		                                                         MAX_PROSE_BYTES);
		if (req.params.count("public"))  is_public = (req.params.find("public")->second == "true");

		std::vector<std::string> paths_to_add;
		std::vector<int> to_remove;
		for (auto& [k, v] : req.params) {
			if (k == "songIdToAdd") {
				if (auto p = store_.song_path_by_id(to_int(v, -1)))
					if (item_read_allowed(req, store_, uploads_root_name_, *p))
						paths_to_add.push_back(*p);
				}
			else if (k == "songIndexToRemove") to_remove.push_back(to_int(v, -1));
			}

		if (!store_.update_playlist(playlist_id, user, name, comment, is_public,
		                             paths_to_add, to_remove)) {
			err(70, "Playlist not found.");
			return;
			}

		std::string body = use_json ? subsonic_ok_json() : subsonic_ok();
		res.set_content(body, use_json ? "application/json" : "application/xml");
		});

	// star
	server_.Get("/rest/star.view", [this](const httplib::Request& req,
	                                      httplib::Response& res) {
		if (!check_auth(req, res, store_)) return;
		bool use_json = (fmt_of(req) == "json");
		std::string user = req.params.find("u")->second;

		// item_read_allowed on every kind of id - see scrobble.
		auto readable = [&](const std::string& path) {
			return item_read_allowed(req, store_, uploads_root_name_, path);
			};
		for (auto& [k, v] : req.params) {
			if (k == "id") {
				if (auto p = store_.song_path_by_id(to_int(v, -1)))
					if (readable(*p)) store_.add_star(user, *p, "", "");
				}
			else if (k == "albumId") {
				if (auto p = store_.album_folder_path_by_id(to_int(v, -1)))
					if (readable(*p)) store_.add_star(user, "", *p, "");
				}
			else if (k == "artistId") {
				if (auto p = store_.artist_folder_path_by_id(to_int(v, -1)))
					if (readable(*p)) store_.add_star(user, "", "", *p);
				}
			}

		std::string body = use_json ? subsonic_ok_json() : subsonic_ok();
		res.set_content(body, use_json ? "application/json" : "application/xml");
		});

	// unstar
	server_.Get("/rest/unstar.view", [this](const httplib::Request& req,
	                                        httplib::Response& res) {
		if (!check_auth(req, res, store_)) return;
		bool use_json = (fmt_of(req) == "json");
		std::string user = req.params.find("u")->second;

		auto readable = [&](const std::string& path) {
			return item_read_allowed(req, store_, uploads_root_name_, path);
			};
		for (auto& [k, v] : req.params) {
			if (k == "id") {
				if (auto p = store_.song_path_by_id(to_int(v, -1)))
					if (readable(*p)) store_.remove_star(user, *p, "", "");
				}
			else if (k == "albumId") {
				if (auto p = store_.album_folder_path_by_id(to_int(v, -1)))
					if (readable(*p)) store_.remove_star(user, "", *p, "");
				}
			else if (k == "artistId") {
				if (auto p = store_.artist_folder_path_by_id(to_int(v, -1)))
					if (readable(*p)) store_.remove_star(user, "", "", *p);
				}
			}

		std::string body = use_json ? subsonic_ok_json() : subsonic_ok();
		res.set_content(body, use_json ? "application/json" : "application/xml");
		});
	// getBookmarks - list all bookmarks for the authenticated user.

	server_.Get("/rest/getBookmarks.view", [this](const httplib::Request& req,
	                                               httplib::Response& res) {
		if (!check_auth(req, res, store_)) return;
		bool use_json = (fmt_of(req) == "json");
		std::string user = req.params.find("u")->second;
		auto bms = store_.get_bookmarks(user);
		int mbr = request_max_bitrate(req, store_);

		std::string body;
		if (use_json)
			body = subsonic_ok_json([&bms, mbr](nlohmann::json& r) {
				nlohmann::json arr = nlohmann::json::array();
				for (auto& bm : bms) {
					nlohmann::json b = {
						{"position", bm.position},
						{"username", bm.username},
						{"comment",  bm.comment},
						{"created",  iso8601(bm.created)},
						{"changed",  iso8601(bm.changed)},
						{"entry",    song_entry_json(bm.entry, mbr)}
						};
					arr.push_back(b);
					}
				r["bookmarks"] = {{"bookmark", arr}};
				});
		else
			body = subsonic_ok([&bms, mbr](XMLDocument& doc, XMLElement* root) {
				auto* bookmarks = doc.NewElement("bookmarks");
				for (auto& bm : bms) {
					auto* b = doc.NewElement("bookmark");
					b->SetAttribute("position", bm.position);
					b->SetAttribute("username", bm.username.c_str());
					b->SetAttribute("comment",  bm.comment.c_str());
					b->SetAttribute("created",  iso8601(bm.created).c_str());
					b->SetAttribute("changed",  iso8601(bm.changed).c_str());
					b->InsertEndChild(song_entry_xml(doc, bm.entry, "entry", mbr));
					bookmarks->InsertEndChild(b);
					}
				root->InsertEndChild(bookmarks);
				});
		res.set_content(body, use_json ? "application/json" : "application/xml");
		});

	// deleteBookmark - remove a bookmark by song id.
	server_.Get("/rest/deleteBookmark.view", [this](const httplib::Request& req,
	                                                 httplib::Response& res) {
		if (!check_auth(req, res, store_)) return;
		bool use_json = (fmt_of(req) == "json");
		std::string user = req.params.find("u")->second;

		auto it = req.params.find("id");
		if (it == req.params.end()) {
			auto msg = "Required parameter missing: id.";
			res.set_content(use_json ? subsonic_error_json(10, msg)
			                         : subsonic_error(10, msg),
			                use_json ? "application/json" : "application/xml");
			return;
			}

		auto song_path = store_.song_path_by_id(to_int(it->second, -1));
		if (!song_path || !store_.delete_bookmark(user, *song_path)) {
			auto msg = "Bookmark not found.";
			res.set_content(use_json ? subsonic_error_json(70, msg)
			                         : subsonic_error(70, msg),
			                use_json ? "application/json" : "application/xml");
			return;
			}

		std::string body = use_json ? subsonic_ok_json() : subsonic_ok();
		res.set_content(body, use_json ? "application/json" : "application/xml");
		});

	// deletePlaylist - remove a playlist owned by the authenticated user.
	server_.Get("/rest/deletePlaylist.view", [this](const httplib::Request& req,
	                                                 httplib::Response& res) {
		if (!check_auth(req, res, store_)) return;
		bool use_json = (fmt_of(req) == "json");
		std::string user = req.params.find("u")->second;

		auto it = req.params.find("id");
		if (it == req.params.end()) {
			auto msg = "Required parameter missing: id.";
			res.set_content(use_json ? subsonic_error_json(10, msg)
			                         : subsonic_error(10, msg),
			                use_json ? "application/json" : "application/xml");
			return;
			}

		if (!store_.delete_playlist(to_int(it->second, -1), user)) {
			auto msg = "Playlist not found.";
			res.set_content(use_json ? subsonic_error_json(70, msg)
			                         : subsonic_error(70, msg),
			                use_json ? "application/json" : "application/xml");
			return;
			}

		std::string body = use_json ? subsonic_ok_json() : subsonic_ok();
		res.set_content(body, use_json ? "application/json" : "application/xml");
		});
	}
