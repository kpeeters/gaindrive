#include "gaindrive.hh"
#include "subsonic.hh"
#include "authz.hh"
#include "stamp.hh"
#include "apientry.hh"
#include "textutil.hh"
#include "untrusted.hh"
#include "imagescale.hh"
#include "codecs.hh"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <map>
#include <vector>

#include <iostream>

#include <tinyxml2.h>
#include <nlohmann/json.hpp>

using namespace tinyxml2;

// Bounds on what one search may ask for. getAlbumList and getRecentSongs have
// clamped for as long as they have existed; search did not, and its LIKE
// pattern can be made to match everything, so the two together turn one
// request into the whole library serialised into a single in-memory document.
static constexpr int MAX_SEARCH_COUNT = 500;

// The matching bound on every paging offset. Unbounded, OFFSET is a full
// table walk inside SQLite - LIMIT 500 OFFSET 100000000 visits every row to
// discard it - executed while holding db_mutex_, behind which every other
// request in the server queues, authentication included. A million deep is
// past the end of any real library and costs a bounded walk.
static constexpr int MAX_LIST_OFFSET = 1000000;


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

// Sorts level-1 folders and groups them into the index buckets `getArtists` and
// `getIndexes` both answer with.
//
// Shared because those two handlers held byte-identical copies of it, differing
// only in the wrapper key of the response - and the first thing anyone edits one
// of them for, they will not think to do twice.
//
// Two groupings, because there are two questions. Normally the bucket is the
// first letter, which is what the alphabet rail and the sticky headers are for.
// When an admin is looking at *everybody's* uploads it is the owner instead:
// that is the only thing telling two identically named folders apart, and
// grouping by letter there would interleave two people's material under one
// heading with nothing to say whose is whose.
static std::map<std::string, std::vector<const MediaStore::ArtistDir*>>
index_buckets(std::vector<MediaStore::ArtistDir>& artists, bool by_owner)
	{
	std::sort(artists.begin(), artists.end(),
		[by_owner](const MediaStore::ArtistDir& a, const MediaStore::ArtistDir& b) {
			// Owner first when grouping by it, so each person's own list is
			// still alphabetical inside their heading.
			if (by_owner && a.owner != b.owner) return a.owner < b.owner;
			return sort_key(a.name) < sort_key(b.name);
			});

	std::map<std::string, std::vector<const MediaStore::ArtistDir*>> buckets;
	for (auto& a : artists) {
		if (by_owner) { buckets[a.owner].push_back(&a); continue; }
		std::string key    = sort_key(a.name);
		std::string letter = key.empty() || !std::isalpha((unsigned char)key[0])
		                   ? "#"
		                   : std::string(1, (char)std::toupper((unsigned char)key[0]));
		buckets[letter].push_back(&a);
		}
	return buckets;
	}


// A scaled cover is buffered and sent with set_content(), never with the
// no-length content provider.  That is framing, not an optimisation: the
// no-length overload emits neither Content-Length nor Transfer-Encoding, and
// httplib only sends Connection: close when the connection is closing for
// unrelated reasons.  The response then goes out on a keep-alive connection
// with nothing marking where the body ends, the client reads on into the
// following response, and every image after the first on that connection is
// the previous one's - which is what "all the thumbnails are wrong, and
// reloading doesn't help" looks like.  The same hazard exists in
// serve_transcoded; this call site cost a long investigation before it got it.
//
// The scaling itself now happens in process (see imagescale.hh) and its result
// is cached (see coverart.hh), so this file no longer runs ffmpeg for images;
// CoverArtCache keeps a fork as a last resort for what stb cannot decode.

// ---- Album list helper -----------------------------------------------

// Shared implementation for getAlbumList and getAlbumList2.
// key is "albumList" or "albumList2".
static void handle_album_list(const httplib::Request& req, httplib::Response& res,
                               MediaStore& store, const char* key)
	{
	bool use_json = (fmt_of(req) == "json");

	auto param_int = [&](const char* name, int def) {
		auto it = req.params.find(name);
		return it != req.params.end() ? to_int(it->second, def) : def;
		};
	auto param_str = [&](const char* name) {
		auto it = req.params.find(name);
		return it != req.params.end() ? it->second : std::string{};
		};

	std::string type     = param_str("type");
	if (type.empty()) type = "newest";
	int size             = std::min(500, std::max(1, param_int("size", 10)));
	int offset           = std::clamp(param_int("offset", 0), 0, MAX_LIST_OFFSET);
	int from_year        = param_int("fromYear", 0);
	int to_year          = param_int("toYear",   0);
	std::string genre    = param_str("genre");
	std::string username = param_str("u");
	bool personal        = param_str("personal") == "true";
	std::string pu       = personal ? username : "";

	auto albums = store.get_album_list(type, size, offset,
	                                   from_year, to_year, genre, username, pu);

	std::string body;
	if (use_json)
		body = subsonic_ok_json([&albums, key](nlohmann::json& r) {
			nlohmann::json arr = nlohmann::json::array();
			for (auto& al : albums) {
				nlohmann::json entry = {
					{"id",        sid(al.id)},
					{"parent",    sid(al.parent_id)},
					{"artistId",  sid(al.parent_id)},
					{"isDir",     true},
					{"title",     al.title},
					{"name",      al.title},
					{"artist",    al.artist},
					{"songCount", al.song_count},
					{"duration",  al.duration},
					{"videoCount", al.video_count}
					};
				if (!al.created.empty()) entry["created"] = iso8601(al.created);
				if (al.cover_art_id >= 0) entry["coverArt"] = sid(al.cover_art_id);
				if (al.year > 0)          entry["year"]     = al.year;
				if (!al.genre.empty())    entry["genre"]    = al.genre;
				if (!al.starred.empty())  entry["starred"]  = iso8601(al.starred);
				arr.push_back(std::move(entry));
				}
			r[key] = {{"album", arr}};
			});
	else
		body = subsonic_ok([&albums, key](XMLDocument& doc, XMLElement* root) {
			auto* list = doc.NewElement(key);
			for (auto& al : albums) {
				auto* el = doc.NewElement("album");
				el->SetAttribute("id",        al.id);
				el->SetAttribute("parent",    al.parent_id);
				el->SetAttribute("isDir",     true);
				el->SetAttribute("title",     al.title.c_str());
				el->SetAttribute("name",      al.title.c_str());
				el->SetAttribute("artist",    al.artist.c_str());
				if (al.cover_art_id >= 0)
					el->SetAttribute("coverArt", al.cover_art_id);
				el->SetAttribute("songCount", al.song_count);
				el->SetAttribute("duration",  al.duration);
				el->SetAttribute("videoCount", al.video_count);
				if (!al.created.empty()) el->SetAttribute("created", iso8601(al.created).c_str());
				if (al.year > 0)         el->SetAttribute("year",    al.year);
				if (!al.genre.empty())   el->SetAttribute("genre",   al.genre.c_str());
				if (!al.starred.empty())
					el->SetAttribute("starred", iso8601(al.starred).c_str());
				list->InsertEndChild(el);
				}
			root->InsertEndChild(list);
			});
	res.set_content(body, use_json ? "application/json" : "application/xml");
	}


void GainDrive::routes_browse()
	{
	// getMusicFolders - returns the configured music root(s).

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
					// contentType is a gaindrive extension: "artists" or "categories".
					arr.push_back({{"id", sid(f.id)}, {"name", f.name},
					               {"contentType", f.type}});
				r["musicFolders"]["musicFolder"] = arr;
				});
		else
			body = subsonic_ok([&folders](XMLDocument& doc, XMLElement* root) {
				auto* mf = doc.NewElement("musicFolders");
				for (auto& f : folders) {
					auto* el = doc.NewElement("musicFolder");
					el->SetAttribute("id",   f.id);
					el->SetAttribute("name", f.name.c_str());
				el->SetAttribute("contentType", f.type.c_str());
					mf->InsertEndChild(el);
					}
				root->InsertEndChild(mf);
				});
		res.set_content(body, use_json ? "application/json" : "application/xml");
		});

	// getIndexes - all artists grouped by first letter.
	server_.Get("/rest/getIndexes.view", [this](const httplib::Request& req,
	                                             httplib::Response& res) {
		if (!check_auth(req, res, store_)) return;

		bool use_json = (fmt_of(req) == "json");
		std::string pu;
		if (!personal_scope(req, res, store_, use_json, pu)) return;
		// musicFolderId restricts to one root; absent means all of them,
		// which is what every existing client sends.
		// contentType is a gaindrive extension: it narrows to a *kind* of
		// root, which musicFolderId cannot express because a kind may span
		// several roots. Absent means every root, mixed, which is what
		// third-party clients expect and get.
		auto artists = store_.get_artist_dirs(
			pu, to_int(req.get_param_value("musicFolderId"), 0),
			req.get_param_value("contentType"));

		auto buckets = index_buckets(artists, pu == MediaStore::PERSONAL_ALL_USERS);

		std::string body;
		if (use_json)
			body = subsonic_ok_json([&buckets](nlohmann::json& r) {
				nlohmann::json idx_arr = nlohmann::json::array();
				for (auto& [letter, vec] : buckets) {
					nlohmann::json artist_arr = nlohmann::json::array();
					for (auto* a : vec)
						artist_arr.push_back({{"id", sid(a->id)}, {"name", a->name},
						                      {"coverArt", sid(a->id)}});
					idx_arr.push_back({{"name", letter}, {"artist", artist_arr}});
					}
				r["indexes"] = {
					{"lastModified",    0},
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
						artist->SetAttribute("id",       a->id);
						artist->SetAttribute("name",     a->name.c_str());
						artist->SetAttribute("coverArt", a->id);
						idx->InsertEndChild(artist);
						}
					indexes->InsertEndChild(idx);
					}
				root->InsertEndChild(indexes);
				});
		res.set_content(body, use_json ? "application/json" : "application/xml");
		});

	// getArtists - same artists as getIndexes but with albumCount per artist.
	server_.Get("/rest/getArtists.view", [this](const httplib::Request& req,
	                                            httplib::Response& res) {
		if (!check_auth(req, res, store_)) return;

		bool use_json = (fmt_of(req) == "json");
		std::string pu;
		if (!personal_scope(req, res, store_, use_json, pu)) return;
		// musicFolderId restricts to one root; absent means all of them,
		// which is what every existing client sends.
		// contentType is a gaindrive extension: it narrows to a *kind* of
		// root, which musicFolderId cannot express because a kind may span
		// several roots. Absent means every root, mixed, which is what
		// third-party clients expect and get.
		auto artists = store_.get_artist_dirs(
			pu, to_int(req.get_param_value("musicFolderId"), 0),
			req.get_param_value("contentType"));

		auto buckets = index_buckets(artists, pu == MediaStore::PERSONAL_ALL_USERS);

		std::string body;
		if (use_json)
			body = subsonic_ok_json([&buckets](nlohmann::json& r) {
				nlohmann::json idx_arr = nlohmann::json::array();
				for (auto& [letter, vec] : buckets) {
					nlohmann::json artist_arr = nlohmann::json::array();
					for (auto* a : vec)
						artist_arr.push_back({{"id", sid(a->id)}, {"name", a->name},
						                      {"albumCount", a->album_count},
						                      {"coverArt",   sid(a->id)}});
					idx_arr.push_back({{"name", letter}, {"artist", artist_arr}});
					}
				r["artists"] = {
					{"lastModified",    0},
					{"ignoredArticles", "The El La Los Las Le Les A An Die Das Ein Eine"},
					{"index",           idx_arr}
					};
				});
		else
			body = subsonic_ok([&buckets](XMLDocument& doc, XMLElement* root) {
				auto* artists_el = doc.NewElement("artists");
				artists_el->SetAttribute("lastModified",    "0");
				artists_el->SetAttribute("ignoredArticles",
					"The El La Los Las Le Les A An Die Das Ein Eine");
				for (auto& [letter, vec] : buckets) {
					auto* idx = doc.NewElement("index");
					idx->SetAttribute("name", letter.c_str());
					for (auto* a : vec) {
						auto* artist = doc.NewElement("artist");
						artist->SetAttribute("id",         a->id);
						artist->SetAttribute("name",       a->name.c_str());
						artist->SetAttribute("albumCount", a->album_count);
						artist->SetAttribute("coverArt",   a->id);
						idx->InsertEndChild(artist);
						}
					artists_el->InsertEndChild(idx);
					}
				root->InsertEndChild(artists_el);
				});
		res.set_content(body, use_json ? "application/json" : "application/xml");
		});

	// getArtist - single artist with album list.
	server_.Get("/rest/getArtist.view", [this](const httplib::Request& req,
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
		const int artist_fid = to_int(it->second, -1);
		if (!check_item_read_perm(req, res, store_, uploads_root_name_,
		                          store_.get_folder_path(artist_fid), use_json)) return;
		auto info = store_.get_artist(artist_fid, user);
		if (!info) { err(70, "Artist not found."); return; }

		std::string body;
		if (use_json)
			body = subsonic_ok_json([&info](nlohmann::json& r) {
				nlohmann::json arr = nlohmann::json::array();
				for (auto& al : info->albums) {
					nlohmann::json entry = {
						{"id",        sid(al.id)},
						{"parent",    sid(al.parent_id)},
						{"artistId",  sid(al.parent_id)},
						{"isDir",     true},
						{"title",     al.title},
						{"name",      al.title},
						{"artist",    al.artist},
						{"songCount", al.song_count},
						{"duration",  al.duration},
						{"videoCount", al.video_count}
						};
					if (!al.created.empty()) entry["created"] = iso8601(al.created);
					if (al.cover_art_id >= 0) entry["coverArt"] = sid(al.cover_art_id);
					if (al.year > 0)          entry["year"]     = al.year;
					if (!al.genre.empty())    entry["genre"]    = al.genre;
					if (!al.starred.empty())  entry["starred"]  = iso8601(al.starred);
					arr.push_back(std::move(entry));
					}
				r["artist"] = {
					{"id",         sid(info->artist.id)},
					{"name",       info->artist.name},
					{"albumCount", info->artist.album_count},
					{"album",      arr}
					};
				});
		else
			body = subsonic_ok([&info](XMLDocument& doc, XMLElement* root) {
				auto* artist_el = doc.NewElement("artist");
				artist_el->SetAttribute("id",         info->artist.id);
				artist_el->SetAttribute("name",       info->artist.name.c_str());
				artist_el->SetAttribute("albumCount", info->artist.album_count);
				for (auto& al : info->albums) {
					auto* el = doc.NewElement("album");
					el->SetAttribute("id",        al.id);
					el->SetAttribute("parent",    al.parent_id);
					el->SetAttribute("isDir",     true);
					el->SetAttribute("title",     al.title.c_str());
					el->SetAttribute("name",      al.title.c_str());
					el->SetAttribute("artist",    al.artist.c_str());
					if (al.cover_art_id >= 0)
						el->SetAttribute("coverArt", al.cover_art_id);
					el->SetAttribute("songCount", al.song_count);
					el->SetAttribute("duration",  al.duration);
					el->SetAttribute("videoCount", al.video_count);
					if (!al.created.empty()) el->SetAttribute("created", iso8601(al.created).c_str());
					if (al.year > 0)         el->SetAttribute("year",    al.year);
					if (!al.genre.empty())   el->SetAttribute("genre",   al.genre.c_str());
					if (!al.starred.empty())
					el->SetAttribute("starred", iso8601(al.starred).c_str());
					artist_el->InsertEndChild(el);
					}
				root->InsertEndChild(artist_el);
				});
		res.set_content(body, use_json ? "application/json" : "application/xml");
		});

	// getMusicDirectory - contents of a folder (album dirs or song files).
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

		const int dir_id = to_int(it->second, -1);
		// Checked on the folder's own path, before the listing is built: the
		// uploads root is skipped by get_music_folders() but is still a
		// folders row, so without this getMusicDirectory on its id lists every
		// user's name, and on a batch id lists that user's library.
		if (!check_item_read_perm(req, res, store_, uploads_root_name_,
		                          store_.get_folder_path(dir_id), use_json)) return;

		auto dir = store_.get_directory(dir_id, flat_multi_disc_);
		if (!dir) { err(70, "Directory not found."); return; }

		int mbr = request_max_bitrate(req, store_);
		std::string body;
		if (use_json)
			body = subsonic_ok_json([&dir, mbr](nlohmann::json& r) {
				nlohmann::json children = nlohmann::json::array();
				for (auto& c : dir->children) {
					nlohmann::json child;
					if (c.is_dir) {
						child = {{"id",sid(c.id)},{"parent",sid(c.parent_id)},
						         {"isDir",true},
						         {"title",c.title},{"artist",c.artist},{"album",c.album}};
						if (c.cover_art_id >= 0) child["coverArt"] = sid(c.cover_art_id);
						if (c.year > 0)          child["year"]     = c.year;
						} else {
						child = song_entry_json(c, mbr);
						}
					children.push_back(child);
					}
				r["directory"] = {
					{"id",    sid(dir->id)},
					{"name",  dir->name},
					{"child", children}
					};
				if (dir->parent_id >= 0)    r["directory"]["parent"]   = sid(dir->parent_id);
				if (dir->cover_art_id >= 0) r["directory"]["coverArt"] = sid(dir->cover_art_id);
				});
		else
			body = subsonic_ok([&dir, mbr](XMLDocument& doc, XMLElement* root) {
				auto* directory = doc.NewElement("directory");
				directory->SetAttribute("id",   dir->id);
				directory->SetAttribute("name", dir->name.c_str());
				if (dir->parent_id >= 0)
					directory->SetAttribute("parent", dir->parent_id);
				if (dir->cover_art_id >= 0)
					directory->SetAttribute("coverArt", dir->cover_art_id);

				for (auto& c : dir->children) {
					XMLElement* child;
					if (c.is_dir) {
						child = doc.NewElement("child");
						child->SetAttribute("id",     c.id);
						child->SetAttribute("parent", c.parent_id);
						child->SetAttribute("isDir",  true);
						child->SetAttribute("title",  c.title.c_str());
						child->SetAttribute("artist", c.artist.c_str());
						child->SetAttribute("album",  c.album.c_str());
						if (c.cover_art_id >= 0)
							child->SetAttribute("coverArt", c.cover_art_id);
						if (c.year > 0)
							child->SetAttribute("year", c.year);
						} else {
						child = song_entry_xml(doc, c, "child", mbr);
						}
					directory->InsertEndChild(child);
					}

				root->InsertEndChild(directory);
				});
		res.set_content(body, use_json ? "application/json" : "application/xml");
		});

	// getAlbumList / getAlbumList2 - both use the same folder-based logic.
	server_.Get("/rest/getAlbumList.view", [this](const httplib::Request& req,
	                                              httplib::Response& res) {
		if (!check_auth(req, res, store_)) return;
		handle_album_list(req, res, store_, "albumList");
		});
	server_.Get("/rest/getAlbumList2.view", [this](const httplib::Request& req,
	                                               httplib::Response& res) {
		if (!check_auth(req, res, store_)) return;
		handle_album_list(req, res, store_, "albumList2");
		});

	// getRecentSongs - gaindrive extension; not in the OpenSubsonic spec.
	// Returns songs ordered by most recently played (per-user play_counts).
	server_.Get("/rest/getRecentSongs.view", [this](const httplib::Request& req,
	                                                httplib::Response& res) {
		if (!check_auth(req, res, store_)) return;

		auto qp = [&](const std::string& k, const std::string& def = "") {
			auto it = req.params.find(k);
			return it != req.params.end() ? it->second : def;
			};

		std::string user = qp("u");
		int size   = std::min(500, std::max(1, to_int(qp("size", "50"), 50)));
		int offset = std::clamp(to_int(qp("offset", "0"), 0), 0, MAX_LIST_OFFSET);

		auto entries = store_.get_recent_songs(user, size, offset);
		int max_br   = request_max_bitrate(req, store_);

		bool use_json = (fmt_of(req) == "json");
		std::string body;
		if (use_json)
			body = subsonic_ok_json([&entries, max_br](nlohmann::json& r) {
				nlohmann::json arr = nlohmann::json::array();
				for (const auto& e : entries) {
					auto s = song_entry_json(e.song, max_br);
					if (!e.last_played.empty()) s["lastPlayed"] = e.last_played;
					arr.push_back(std::move(s));
					}
				r["recentSongs"] = {{"song", arr}};
				});
		else
			body = subsonic_ok([&entries, max_br](XMLDocument& doc, XMLElement* root) {
				auto* rs = doc.NewElement("recentSongs");
				for (const auto& e : entries) {
					auto* el = song_entry_xml(doc, e.song, "song", max_br);
					if (!e.last_played.empty())
						el->SetAttribute("lastPlayed", e.last_played.c_str());
					rs->InsertEndChild(el);
					}
				root->InsertEndChild(rs);
				});
		res.set_content(body, use_json ? "application/json" : "application/xml");
		});

	// getNowPlaying: who is listening right now.  Backed by client.now_playing,
	// which scrobble(submission=false) refreshes; get_now_playing() only
	// returns rows younger than five minutes.  Entries under another user's
	// private uploads are dropped whole, since the entry *is* the song and
	// serving it would leak what the listing hides.
	server_.Get("/rest/getNowPlaying.view", [this](const httplib::Request& req,
	                                               httplib::Response& res) {
		if (!check_auth(req, res, store_)) return;

		auto entries = store_.get_now_playing();
		int max_br   = request_max_bitrate(req, store_);

		std::erase_if(entries, [&](const MediaStore::NowPlayingEntry& e) {
			return !item_read_allowed(req, store_, uploads_root_name_, e.song_path);
			});

		bool use_json = (fmt_of(req) == "json");
		std::string body;
		if (use_json)
			body = subsonic_ok_json([&entries, max_br](nlohmann::json& r) {
				nlohmann::json arr = nlohmann::json::array();
				int idx = 0;
				for (const auto& e : entries) {
					auto s = song_entry_json(e.song, max_br);
					s["username"]   = e.username;
					s["minutesAgo"] = e.minutes_ago;
					s["playerId"]   = ++idx;
					if (!e.client.empty()) s["playerName"] = e.client;
					arr.push_back(std::move(s));
					}
				r["nowPlaying"] = {{"entry", arr}};
				});
		else
			body = subsonic_ok([&entries, max_br](XMLDocument& doc, XMLElement* root) {
				auto* np = doc.NewElement("nowPlaying");
				int idx = 0;
				for (const auto& e : entries) {
					auto* el = song_entry_xml(doc, e.song, "entry", max_br);
					el->SetAttribute("username", e.username.c_str());
					el->SetAttribute("minutesAgo", e.minutes_ago);
					el->SetAttribute("playerId", ++idx);
					if (!e.client.empty())
						el->SetAttribute("playerName", e.client.c_str());
					np->InsertEndChild(el);
					}
				root->InsertEndChild(np);
				});
		res.set_content(body, use_json ? "application/json" : "application/xml");
		});

	// getArtistInfo / getArtistInfo2 - answered from artist_info_cache, with the
	// lookup itself queued onto the info resolver. Both endpoints share
	// identical logic; only the response key name differs.
	server_.Get("/rest/getArtistInfo.view", [this](const httplib::Request& req,
	                                               httplib::Response& res) {
		if (!check_auth(req, res, store_)) return;
		handle_artist_info(req, res, "artistInfo");
		});
	server_.Get("/rest/getArtistInfo2.view", [this](const httplib::Request& req,
	                                                httplib::Response& res) {
		if (!check_auth(req, res, store_)) return;
		handle_artist_info(req, res, "artistInfo2");
		});

	// getCoverArt - serve a cover image, optionally scaled.
	server_.Get("/rest/getCoverArt.view", [this](const httplib::Request& req,
	                                              httplib::Response& res) {
		auto it = req.params.find("id");
		if (it == req.params.end()) {
			res.set_content(subsonic_error(10, "Required parameter missing: id."),
			                "application/xml");
			return;
			}

		int folder_id = to_int(it->second, -1);

		// The id is read before the token, as stream.view reads its own first
		// and for the same reason: a grant authorises *this* cover or none.
		//
		// A receiver fetching the sleeve a LOAD named has no account, and this
		// is the whole reason a grant covers artwork rather than stopping at
		// the audio: the picture travels in the LOAD's metadata, the receiver
		// goes and gets it, and a grant that did not cover it would leave the
		// account's password on the television anyway. The server's own cast
		// never needed this because it sends no artwork at all; the clients
		// that hold their own control channel do.
		auto tok_it = req.params.find("castToken");
		const bool grant_authed = tok_it != req.params.end()
		                       && grant_allows_cover(tok_it->second, folder_id);
		if (!grant_authed && !check_auth(req, res, store_)) return;

		std::string rel_path = store_.get_cover_path(folder_id);

		// A cover is as personal as the item it belongs to, and every stored
		// path begins with its root's name - so `rel_path` answers the question
		// on its own whenever there is one, whether it names an image, a loose
		// file's sidecar or a video the art was extracted from.
		//
		// **Deliberately not an extra get_folder_path() here.** This is the
		// album-grid hot path - a client asks for every cover at once and each
		// query takes db_mutex_, which a scan holds across whole album
		// transactions - so the only branch that costs a lookup is the one
		// with no cover_path at all, an artist folder, which the handler is
		// about to look up anyway.
		//
		// A grant skips it, with the same safety stream.view relies on: the
		// check was run against its account at getCastToken, and the grant
		// names the one cover that song passed for.
		if (!grant_authed
		    && !check_item_read_perm(req, res, store_, uploads_root_name_,
		                             rel_path.empty()
		                                 ? store_.get_folder_path(folder_id)
		                                 : rel_path,
		                             fmt_of(req) == "json")) return;

		namespace fs = std::filesystem;

		// The size the client asked for, rounded to the ladder - see
		// CoverArtCache::ladder_size. 0 means "serve the source".
		auto size_it = req.params.find("size");
		int  ladder  = size_it != req.params.end()
		    ? CoverArtCache::ladder_size(to_int(size_it->second, 200)) : 0;

		// Optional index: 0 (default) = main cover, 1+ = extra images sorted.
		auto idx_it = req.params.find("index");

		// Resolve the id into something with bytes, a key and a stamp. The
		// three kinds of art differ only here; everything below is shared, so
		// a video poster and an artist portrait are scaled, cached and
		// revalidated by exactly the code that handles an ordinary cover.
		CoverArtCache::Source src;
		std::string           orig_mime;
		int64_t               orig_len = 0;

		if (rel_path.empty()) {
			// An artist folder. Never fall back to album cover art: an artist
			// showing one of their album covers as a portrait looks like a bug
			// in whichever client drew it.
			//
			// This reads the database and nothing else. It used to run the
			// whole MusicBrainz -> Wikidata -> Wikipedia -> TheAudioDB ->
			// Discogs chain right here, in the request thread, pacing sleeps
			// included - so a client showing a grid of artists could hold the
			// entire HTTP pool in network waits.
			std::string fpath = store_.get_folder_path(folder_id);
			std::string name  = store_.get_folder_name(folder_id);
			if (fpath.empty() || name.empty()) {
				res.status = 404;
				return;
				}
			// **An album is not an artist, even when it has no cover.**  This
			// branch is reached whenever get_cover_path() came back empty, and
			// an album folder with no image at all lands here as readily as a
			// real artist folder does - so a coverless album has always been
			// answered with an artist portrait and pushed onto the MusicBrainz
			// queue under its own title.  A pre-existing bug, but one this had
			// to grow a guard for: a loose file is its own album now, so every
			// loose track without a sidecar image would ask MusicBrainz about
			// "Track.mp4".
			//
			// The extra lookup is affordable because it is inside the already
			// cold empty-cover branch, which does two of its own; the album
			// grid's hot path never reaches it.
			if (store_.folder_is_album(folder_id)) {
				res.status = 404;
				return;
				}
			auto state = store_.get_artist_art_state(fpath);

			if (!state || state->status == "error") {
				// Not resolved yet, or the network failed last time. Push this
				// artist to the front of the queue - what somebody is looking
				// at beats the alphabet - and say so at once.
				//
				// no-store, not no-cache: a client must be able to re-ask in a
				// few seconds and get the picture. A cached 404 is how "the
				// portraits never appear until you restart the browser" would
				// happen.
				if (!store_.is_category_folder(folder_id))
					lookup_request_front(LookupKind::Artist, folder_id, fpath, name);
				res.set_header("Cache-Control", "no-store");
				res.status = 404;
				return;
				}
			if (state->status != "ok") {
				// "none": every provider was asked and none had a picture.
				// Cacheable on purpose - a client retrying on a timer would
				// otherwise poll for ever over an artist nobody has a portrait
				// of, and on a real library that is many of them.
				//
				// An hour, not a day. This is the one answer here a client is
				// allowed to keep without asking, so its lifetime is also how
				// long a *wrong* "none" survives on the device after the
				// server has stopped believing it - and a wrong one is
				// entirely possible, since it is what a provider outage looks
				// like if anything upstream mistakes silence for a verdict.
				// Deleting the row server-side cannot reach a cache on a
				// phone. An hour still silences any retry loop; a day meant a
				// correction took a day to arrive.
				res.set_header("Cache-Control", "public, max-age=3600");
				res.status = 404;
				return;
				}

			auto art = store_.get_artist_art(fpath);
			if (!art) {
				res.status = 404;
				return;
				}
			src.kind  = CoverArtCache::Source::Kind::Blob;
			src.key   = fpath;
			src.stamp = art->fetched_at;
			orig_len  = static_cast<int64_t>(art->bytes.size());
			orig_mime = art->mime.empty() ? "image/jpeg" : art->mime;
			src.bytes = std::move(art->bytes);
			}
		else {
			if (idx_it != req.params.end()) {
				int idx = to_int(idx_it->second, 0);
				if (idx > 0) {
					auto extras = store_.get_extra_image_paths(folder_id);
					if (idx - 1 >= static_cast<int>(extras.size())) {
						res.status = 404;
						return;
						}
					rel_path = extras[idx - 1];
					}
				}

			// A cover_path naming a *media* file means the art was manufactured
			// from that file and lives in the video_art table - see videoart.hh.
			// Storing the media path rather than inventing a marker is what lets
			// every cover-art query, and the whole web client, stay unchanged.
			if (is_video_ext(ext_of(rel_path))) {
				auto art = store_.get_video_art(rel_path);
				if (!art) {
					// no-store, as the artist branch above: the poster may be
					// seconds away (a rename refresh, a scan in flight), and a
					// heuristically cached 404 pins the blank tile until a
					// browser restart.
					res.set_header("Cache-Control", "no-store");
					res.status = 404;
					return;
					}
				src.kind  = CoverArtCache::Source::Kind::Blob;
				src.key   = rel_path;
				// created_at, not file_modified: the media's mtime does not
				// move when a poster is replaced, so a same-length
				// replacement would 304 its way to the stale image.
				src.stamp = art->created_at;
				orig_len  = static_cast<int64_t>(art->bytes.size());
				orig_mime = art->mime.empty() ? "image/jpeg" : art->mime;
				src.bytes = std::move(art->bytes);
				}
			else {
				// Compose absolute filesystem path for the actual file open.
				std::string path = store_.abs_path(rel_path);
				if (!store_.path_is_within_root(path)) {
					std::cout << stamp()
					          << "getCoverArt: refusing path outside every root: "
					          << path << std::endl;
					res.status = 403;
					return;
					}
				std::error_code mec, sec;
				auto  mtime = fs::last_write_time(path, mec);
				auto  fsize = fs::file_size(path, sec);
				// Each stat gets its own error_code, and both are resolved before
				// they are read: sharing one would read and write it within a
				// single unsequenced expression.
				src.kind     = CoverArtCache::Source::Kind::File;
				src.key      = rel_path;
				src.abs_path = path;
				src.stamp    = mec ? 0 : static_cast<int64_t>(
					mtime.time_since_epoch().count());
				orig_len     = sec ? 0 : static_cast<int64_t>(fsize);
				}
			}

		// Revalidate rather than trust the cache.  A cover URL is identified
		// only by `id`, and folders.id is a rowid that is NOT stable across a
		// rescan - which is exactly why stars and playlists key on paths
		// instead.  Without a validator a browser caches the image
		// heuristically and indefinitely, so after a rebuild reassigns ids it
		// keeps showing the previous album's art with no way to notice.
		// "no-cache" means "keep it, but check first", so this stays fast.
		//
		// The "t2-" is a scheme marker, and it is not decoration: a client
		// holds ETags for bytes made under the *previous* scaling rule, and
		// the same URL now answers with different ones. Without a marker a
		// browser would 304 its way into keeping the old thumbnail for ever.
		// Bump it if the encoder, the quality, the ladder or the fit rule
		// changes again. t1- was ffmpeg's output, then stb fitting the long
		// edge; t2- is stb fitting the short one.
		//
		// This is the client half. The server holds its own copies in
		// cover_thumbs, keyed on nothing the fit rule moves, so they are
		// dropped by the cache-scheme user_version in MediaStore instead.
		//
		// The *ladder* value goes in, not what the client asked for: two
		// requests that round to the same rung are the same bytes and must
		// share a validator.
		std::string etag = "\"t2-" + std::to_string(src.stamp)
		    + "-" + std::to_string(orig_len)
		    + "-" + (ladder > 0 ? std::to_string(ladder) : std::string("full"))
		    + "-" + std::to_string(idx_it != req.params.end()
		                           ? to_int(idx_it->second, 0) : 0) + "\"";
		res.set_header("Cache-Control", "no-cache");
		res.set_header("ETag", etag);
		if (req.get_header_value("If-None-Match") == etag) {
			res.status = 304;
			return;
			}

		if (ladder > 0) {
			// nullopt means the source is already small enough, or nothing
			// could decode it. Both fall through to serving it whole, and both
			// were recorded, so neither is worked out again.
			if (auto out = cover_cache_.scaled(src, ladder)) {
				res.set_content(out->bytes, out->mime);
				return;
				}
			}

		if (src.kind == CoverArtCache::Source::Kind::Blob) {
			res.set_content(src.bytes, orig_mime);
			return;
			}

		// Serve the full-size image directly. This keeps the length-carrying
		// content provider rather than buffering: a full-size cover can be
		// twenty megabytes, and no UI asks for one - only the lightbox does.
		//
		// The MIME comes from the magic bytes. It used to be image/jpeg
		// whatever was on disk, which every browser renders anyway and every
		// cache and proxy in between believes.
		std::string head;
		{
		std::ifstream hf(src.abs_path, std::ios::binary);
		char          hb[12];
		if (hf) {
			hf.read(hb, sizeof(hb));
			head.assign(hb, static_cast<size_t>(hf.gcount()));
			}
		}
		std::string mime = imagescale::sniff_mime(head);
		if (mime.empty()) mime = "application/octet-stream";

		std::string path = src.abs_path;
		res.set_content_provider(
			static_cast<size_t>(orig_len), mime,
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

	// getAlbumTexts - list .txt files in an album folder.
	server_.Get("/rest/getAlbumTexts.view", [this](const httplib::Request& req,
	                                               httplib::Response& res) {
		if (!check_auth(req, res, store_)) return;

		auto it = req.params.find("id");
		if (it == req.params.end()) {
			res.set_content(subsonic_error_json(10, "Required parameter missing: id."),
			                "application/json");
			return;
			}

		std::string folder_rel = store_.get_folder_path(to_int(it->second, -1));
		if (!check_item_read_perm(req, res, store_, uploads_root_name_,
		                          folder_rel, fmt_of(req) == "json")) return;
		if (folder_rel.empty()) {
			res.status = 404;
			return;
			}
		std::string folder = store_.abs_path(folder_rel);
		if (!store_.path_is_within_root(folder)) {
			std::cout << stamp() << "getAlbumTexts: refusing path outside every root: "
			          << folder << std::endl;
			res.status = 403;
			return;
			}

		// Well-known utility files that are not human-readable liner notes.
		static const std::set<std::string> excluded = {
			"fingerprints.txt",
			};
		// Chapter sidecars are matched by suffix rather than by name, since
		// the stem is the video's. The constant is MediaStore's so there is
		// one spelling of it: this listing and sidecar_chapters_path() are the
		// two places that know the name, and the day they disagreed chapter
		// files would silently start appearing as prose.
		//
		// Note the ".part" a save writes first needs no entry -- it fails the
		// ".txt" extension test below. That is luck rather than design, so do
		// not lean on it if that test ever loosens.
		const std::string chapters_suffix(MediaStore::CHAPTERS_SUFFIX);

		namespace fs = std::filesystem;
		nlohmann::json files = nlohmann::json::array();
		std::error_code fec;
		if (fs::is_regular_file(folder, fec)) {
			// A file-album: its liner notes are the sidecar named after it,
			// <stem>.txt, and there is no directory to list.  Handled
			// explicitly rather than left to the directory_iterator below
			// throwing into the catch - which returns the right answer for the
			// wrong reason, and returns *nothing* where a real note exists.
			//
			// It cannot collide with the chapter sidecar: that is
			// <stem>.chapters.txt, a different filename, which is the whole
			// point of sidecar_chapters_path() not being replace_extension().
			std::string note = MediaStore::sidecar_text_path(folder);
			if (fs::exists(note, fec)) {
				// utf8_clean before json: a filename is arbitrary bytes on
				// Linux, and one invalid sequence makes dump() throw - a
				// permanent 500 for this album's listing. A name the clean
				// changes cannot be fetched back through getAlbumText anyway,
				// so it is dropped rather than listed under a lie.
				const std::string fname = fs::path(note).filename().string();
				const std::string clean = utf8_clean(fname, 255);
				if (clean == fname)
					files.push_back({{"name", clean}});
				}
			}
		else try {
			for (auto& entry : fs::directory_iterator(folder)) {
				if (!entry.is_regular_file() || entry.path().extension() != ".txt") continue;
				if (is_hidden_name(entry.path())) continue;
				const std::string fname = entry.path().filename().string();
				if (excluded.count(fname)) continue;
				if (fname.size() > chapters_suffix.size()
				        && fname.compare(fname.size() - chapters_suffix.size(),
				                         chapters_suffix.size(),
				                         chapters_suffix) == 0)
					continue;
				// Same rule as the file-album branch above.
				const std::string clean = utf8_clean(fname, 255);
				if (clean != fname) continue;
				files.push_back({{"name", clean}});
				}
			}
		catch (...) {}

		// Sort alphabetically so the order is stable.
		std::sort(files.begin(), files.end(), [](const nlohmann::json& a, const nlohmann::json& b){
			return a["name"].get<std::string>() < b["name"].get<std::string>();
			});

		res.set_content(subsonic_ok_json([&files](nlohmann::json& r) {
			r["albumTexts"] = {{"textFile", files}};
			}), "application/json");
		});

	// getAlbumImages - return total image count for an album folder (cover + extras).
	server_.Get("/rest/getAlbumImages.view", [this](const httplib::Request& req,
	                                                httplib::Response& res) {
		if (!check_auth(req, res, store_)) return;

		auto it = req.params.find("id");
		if (it == req.params.end()) {
			res.set_content(subsonic_error_json(10, "Required parameter missing: id."),
			                "application/json");
			return;
			}

		const int images_fid = to_int(it->second, -1);
		if (!check_item_read_perm(req, res, store_, uploads_root_name_,
		                          store_.get_folder_path(images_fid),
		                          fmt_of(req) == "json")) return;
		int count = store_.get_image_count(images_fid);
		res.set_content(subsonic_ok_json([count](nlohmann::json& r) {
			r["albumImages"] = {{"count", count}};
			}), "application/json");
		});

	// getAlbumText - serve a single .txt file from an album folder.
	server_.Get("/rest/getAlbumText.view", [this](const httplib::Request& req,
	                                              httplib::Response& res) {
		if (!check_auth(req, res, store_)) return;

		auto id_it   = req.params.find("id");
		auto name_it = req.params.find("name");
		if (id_it == req.params.end() || name_it == req.params.end()) {
			res.status = 400;
			return;
			}

		// Reject any path traversal attempts. The NUL test is not
		// theoretical: a query parameter arrives percent-decoded, so
		// `cover.jpg%00.txt` passes the suffix test as a C++ string and is
		// then truncated at the NUL by every c_str() the filesystem layer
		// takes - the file opened is cover.jpg, served as text/plain.
		const std::string& name = name_it->second;
		if (name.find('/') != std::string::npos  ||
		    name.find('\\') != std::string::npos ||
		    name.find('\0') != std::string::npos ||
		    name.find("..") != std::string::npos ||
		    name.size() < 5 ||
		    name.substr(name.size() - 4) != ".txt") {
			res.status = 400;
			return;
			}

		std::string folder_rel = store_.get_folder_path(to_int(id_it->second, -1));
		if (!check_item_read_perm(req, res, store_, uploads_root_name_,
		                          folder_rel, fmt_of(req) == "json")) return;
		if (folder_rel.empty()) {
			res.status = 404;
			return;
			}

		namespace fs = std::filesystem;
		// A file-album has no directory to compose against, so the note is
		// taken from beside the media file - and `name` has to *be* that
		// sidecar's filename.  Composing from the parent directory instead
		// would let this endpoint read any album's notes out of the section
		// the file happens to sit in, which is a different album's content.
		std::string album_abs = store_.abs_path(folder_rel);
		std::error_code fec;
		fs::path full;
		if (fs::is_regular_file(album_abs, fec)) {
			fs::path note = MediaStore::sidecar_text_path(album_abs);
			if (note.filename().string() != name) {
				res.status = 404;
				return;
				}
			full = note;
			}
		else
			full = fs::path(album_abs) / name;
		if (!store_.path_is_within_root(full)) {
			std::cout << stamp() << "getAlbumText: refusing path outside every root: "
			          << full.string() << std::endl;
			res.status = 403;
			return;
			}
		// Bounded before it is read: liner notes are kilobytes, and the file
		// is whatever an uploader put beside the album - an 8 GiB .txt inside
		// an archive would otherwise become an 8 GiB allocation per request.
		std::error_code sec;
		const auto fsize = fs::file_size(full, sec);
		if (sec) { res.status = 404; return; }
		if (fsize > MAX_SMALL_BODY_BYTES) {
			res.status = 413;
			return;
			}

		std::ifstream f(full);
		if (!f) {
			res.status = 404;
			return;
			}

		std::string content((std::istreambuf_iterator<char>(f)),
		                     std::istreambuf_iterator<char>());
		res.set_content(content, "text/plain; charset=utf-8");
		});
	// getAlbum - single album with its track list.

	server_.Get("/rest/getAlbum.view", [this](const httplib::Request& req,
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
		const int album_fid = to_int(it->second, -1);
		const std::string album_rel = store_.get_folder_path(album_fid);
		if (!check_item_read_perm(req, res, store_, uploads_root_name_,
		                          album_rel, use_json)) return;
		auto info = store_.get_album(album_fid, flat_multi_disc_, user);
		if (!info) { err(70, "Album not found."); return; }
		int mbr = request_max_bitrate(req, store_);
		// Reported for the same reason chapters.writable is: the client draws an
		// Edit affordance from this rather than guessing at the rule, which it
		// cannot do - an album reached from search or from the player carries no
		// trace of whether it came out of the caller's own uploads.
		const bool writable = item_write_allowed(req, store_, uploads_root_name_,
		                                         album_rel);

		std::string body;
		if (use_json)
			body = subsonic_ok_json([&info, mbr, writable](nlohmann::json& r) {
				nlohmann::json songs = nlohmann::json::array();
				for (auto& s : info->songs)
					songs.push_back(song_entry_json(s, mbr));
				auto& al = info->album;
				nlohmann::json entry = {
					{"id",        sid(al.id)},
					{"parent",    sid(al.parent_id)},
					{"artistId",  sid(al.parent_id)},
					{"name",      al.title},
					{"artist",    al.artist},
					{"songCount", al.song_count},
					{"duration",  al.duration},
					{"videoCount", al.video_count},
					{"writable",  writable},
					{"song",      songs}
					};
				if (!al.created.empty()) entry["created"] = iso8601(al.created);
				if (al.cover_art_id >= 0) entry["coverArt"] = sid(al.cover_art_id);
				if (al.year > 0)          entry["year"]     = al.year;
				if (!al.genre.empty())    entry["genre"]    = al.genre;
				if (!al.starred.empty())  entry["starred"]  = iso8601(al.starred);
				r["album"] = std::move(entry);
				});
		else
			body = subsonic_ok([&info, mbr, writable](XMLDocument& doc, XMLElement* root) {
				auto& al = info->album;
				auto* el = doc.NewElement("album");
				el->SetAttribute("id",        al.id);
				el->SetAttribute("parent",    al.parent_id);
				el->SetAttribute("name",      al.title.c_str());
				el->SetAttribute("artist",    al.artist.c_str());
				el->SetAttribute("songCount", al.song_count);
				el->SetAttribute("duration",  al.duration);
				el->SetAttribute("videoCount", al.video_count);
				el->SetAttribute("writable",  writable);
				if (al.cover_art_id >= 0) el->SetAttribute("coverArt", al.cover_art_id);
				if (!al.created.empty())  el->SetAttribute("created",  iso8601(al.created).c_str());
				if (al.year > 0)          el->SetAttribute("year",     al.year);
				if (!al.genre.empty())    el->SetAttribute("genre",    al.genre.c_str());
				if (!al.starred.empty())
					el->SetAttribute("starred", iso8601(al.starred).c_str());
				for (auto& s : info->songs)
					el->InsertEndChild(song_entry_xml(doc, s, "song", mbr));
				root->InsertEndChild(el);
				});
		res.set_content(body, use_json ? "application/json" : "application/xml");
		});

	// getAlbumInfo / getAlbumInfo2 - MusicBrainz lookup, result cached in DB.
	// Both endpoints share identical logic; only the response key name differs.
	// An album id here *is* a folder id, so there is no id3 album to look up
	// separately and the older name answers the same question as the newer.
	server_.Get("/rest/getAlbumInfo.view", [this](const httplib::Request& req,
	                                              httplib::Response& res) {
		if (!check_auth(req, res, store_)) return;
		handle_album_info(req, res, "albumInfo");
		});
	server_.Get("/rest/getAlbumInfo2.view", [this](const httplib::Request& req,
	                                               httplib::Response& res) {
		if (!check_auth(req, res, store_)) return;
		handle_album_info(req, res, "albumInfo2");
		});

	// getTopSongs - play-count tracking not implemented; return empty list.
	server_.Get("/rest/getTopSongs.view", [this](const httplib::Request& req,
	                                             httplib::Response& res) {
		if (!check_auth(req, res, store_)) return;
		bool use_json = (fmt_of(req) == "json");
		std::string body;
		if (use_json)
			body = subsonic_ok_json([](nlohmann::json& r) {
				r["topSongs"] = {{"song", nlohmann::json::array()}};
				});
		else
			body = subsonic_ok([](XMLDocument& doc, XMLElement* root) {
				root->InsertEndChild(doc.NewElement("topSongs"));
				});
		res.set_content(body, use_json ? "application/json" : "application/xml");
		});

	// getSong - full metadata for a single track.
	server_.Get("/rest/getSong.view", [this](const httplib::Request& req,
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

		auto song = store_.get_song_entry(to_int(it->second, -1));
		if (!song) { err(70, "Song not found."); return; }
		if (!check_item_read_perm(req, res, store_, uploads_root_name_,
		                          song->path, use_json)) return;
		int mbr = request_max_bitrate(req, store_);

		std::string body;
		if (use_json)
			body = subsonic_ok_json([&song, mbr](nlohmann::json& r) {
				r["song"] = song_entry_json(*song, mbr);
				});
		else
			body = subsonic_ok([&song, mbr](XMLDocument& doc, XMLElement* root) {
				root->InsertEndChild(song_entry_xml(doc, *song, "song", mbr));
				});
		res.set_content(body, use_json ? "application/json" : "application/xml");
		});

	// getGenres - every distinct genre in the shared library, with counts.
	//
	// The tags are reported as they are, typos and "unknown" included: this
	// endpoint is the only view anyone gets of their own tagging, and a list
	// filtered down to the plausible entries would hide the very thing that
	// needs fixing. Only case and padding are folded away, which loses
	// nothing. See MediaStore::get_genres().
	server_.Get("/rest/getGenres.view", [this](const httplib::Request& req,
	                                            httplib::Response& res) {
		if (!check_auth(req, res, store_)) return;
		bool use_json = (fmt_of(req) == "json");
		auto genres   = store_.get_genres();

		std::string body;
		if (use_json)
			body = subsonic_ok_json([&genres](nlohmann::json& r) {
				nlohmann::json arr = nlohmann::json::array();
				for (auto& g : genres)
					arr.push_back({{"value",      g.name},
					               {"songCount",  g.song_count},
					               {"albumCount", g.album_count}});
				r["genres"] = {{ "genre", arr }};
				});
		else
			body = subsonic_ok([&genres](XMLDocument& doc, XMLElement* root) {
				auto* el = doc.NewElement("genres");
				for (auto& g : genres) {
					auto* ge = doc.NewElement("genre");
					ge->SetAttribute("songCount",  g.song_count);
					ge->SetAttribute("albumCount", g.album_count);
					// The name is the element's text, not an attribute --
					// Subsonic spells this one differently from every other
					// listing, and a client reading it as `name` gets nothing.
					ge->SetText(g.name.c_str());
					el->InsertEndChild(ge);
					}
				root->InsertEndChild(el);
				});
		res.set_content(body, use_json ? "application/json" : "application/xml");
		});

	// getSongsByGenre - the songs carrying one genre.
	server_.Get("/rest/getSongsByGenre.view", [this](const httplib::Request& req,
	                                                  httplib::Response& res) {
		if (!check_auth(req, res, store_)) return;
		bool use_json = (fmt_of(req) == "json");
		auto err = [&](int code, const char* msg) {
			if (use_json)
				res.set_content(subsonic_error_json(code, msg), "application/json");
			else
				res.set_content(subsonic_error(code, msg),      "application/xml");
			};

		std::string genre = req.get_param_value("genre");
		if (genre.empty()) { err(10, "Required parameter missing: genre."); return; }

		// Clamped like search: count is an unvalidated client integer and the
		// whole response is built in memory as one document.
		int count  = std::clamp(to_int(req.get_param_value("count"), 10),
		                        1, MAX_SEARCH_COUNT);
		int offset = std::clamp(to_int(req.get_param_value("offset"), 0),
		                        0, MAX_LIST_OFFSET);

		auto songs = store_.get_songs_by_genre(genre, count, offset);
		int  mbr   = request_max_bitrate(req, store_);

		std::string body;
		if (use_json)
			body = subsonic_ok_json([&songs, mbr](nlohmann::json& r) {
				nlohmann::json entries = nlohmann::json::array();
				for (auto& s : songs)
					entries.push_back(song_entry_json(s, mbr));
				r["songsByGenre"] = {{ "song", entries }};
				});
		else
			body = subsonic_ok([&songs, mbr](XMLDocument& doc, XMLElement* root) {
				auto* el = doc.NewElement("songsByGenre");
				for (auto& s : songs)
					el->InsertEndChild(song_entry_xml(doc, s, "song", mbr));
				root->InsertEndChild(el);
				});
		res.set_content(body, use_json ? "application/json" : "application/xml");
		});
	// search2 / search3 - title/name substring search across artists, albums, songs.
	// Both share identical logic; only the response envelope key differs.

	auto search_handler = [this](const httplib::Request& req, httplib::Response& res,
	                              const char* key) {
		if (!check_auth(req, res, store_)) return;
		bool use_json = (fmt_of(req) == "json");

		auto qp = [&](const std::string& k, const std::string& def = "") {
			auto it = req.params.find(k);
			return it != req.params.end() ? it->second : def;
			};

		auto err = [&](int code, const char* msg) {
			if (use_json)
				res.set_content(subsonic_error_json(code, msg), "application/json");
			else
				res.set_content(subsonic_error(code, msg),      "application/xml");
			};

		std::string query = qp("query");
		if (query.empty()) { err(10, "Required parameter missing: query."); return; }

		// Clamped as handle_album_list and getRecentSongs already clamp, and
		// through to_int so a non-numeric count is a default rather than a 500.
		auto count_param = [&](const char* k) {
			return std::clamp(to_int(qp(k, "20"), 20), 0, MAX_SEARCH_COUNT);
			};
		auto offset_param = [&](const char* k) {
			return std::clamp(to_int(qp(k, "0"), 0), 0, MAX_LIST_OFFSET);
			};
		int artist_count  = count_param("artistCount");
		int artist_offset = offset_param("artistOffset");
		int album_count   = count_param("albumCount");
		int album_offset  = offset_param("albumOffset");
		int song_count    = count_param("songCount");
		int song_offset   = offset_param("songOffset");
		// Defaults to 0, unlike its three siblings: a client that does not
		// know about chapters must not be made to pay for a fourth scan, and
		// one that does asks for them by name.
		int chapter_count  = std::clamp(to_int(qp("chapterCount", "0"), 0),
		                                0, MAX_SEARCH_COUNT);
		int chapter_offset = offset_param("chapterOffset");

		bool personal = qp("personal") == "true";
		std::string pu = personal ? qp("u") : "";
		auto sr = store_.search(query,
		                        artist_count, artist_offset,
		                        album_count,  album_offset,
		                        song_count,   song_offset,
		                        chapter_count, chapter_offset,
		                        pu);
		int mbr = request_max_bitrate(req, store_);

		std::string body;
		if (use_json)
			body = subsonic_ok_json([&sr, key, mbr](nlohmann::json& r) {
				nlohmann::json artists = nlohmann::json::array();
				for (auto& a : sr.artists)
					artists.push_back({{"id", sid(a.id)}, {"name", a.title}});

				nlohmann::json albums = nlohmann::json::array();
				for (auto& c : sr.albums) {
					nlohmann::json al = {
						{"id",     sid(c.id)},
						{"parent", sid(c.parent_id)},
						{"isDir",  true},
						{"title",  c.title},
						{"artist", c.artist},
						{"album",  c.title}
						};
					if (c.cover_art_id >= 0) al["coverArt"] = sid(c.cover_art_id);
					albums.push_back(al);
					}

				nlohmann::json songs = nlohmann::json::array();
				for (auto& s : sr.songs)
					songs.push_back(song_entry_json(s, mbr));

				// A list of its own, never entries in `song`. A chapter has
				// no id anything can stream, star or queue, so a client told
				// it was a song would be handed a track that does not work.
				nlohmann::json chapters = nlohmann::json::array();
				for (auto& h : sr.chapters)
					chapters.push_back({
						{"songId",   sid(h.song_id)},
						{"parent",   sid(h.parent_id)},
						{"index",    h.index},
						{"start",    std::llround(h.start * 1000.0) / 1000.0},
						{"name",     h.name},
						{"track",    h.track},
						{"album",    h.album},
						{"artist",   h.artist} });

				r[key] = {{"artist", artists}, {"album", albums},
				          {"song", songs}};
				// Absent rather than empty when it was not asked for, so a
				// client that never sends chapterCount sees the response it
				// has always seen.
				if (!sr.chapters.empty()) r[key]["chapter"] = chapters;
				});
		else
			body = subsonic_ok([&sr, key, mbr](XMLDocument& doc, XMLElement* root) {
				auto* result = doc.NewElement(key);

				for (auto& a : sr.artists) {
					auto* el = doc.NewElement("artist");
					el->SetAttribute("id",   a.id);
					el->SetAttribute("name", a.title.c_str());
					result->InsertEndChild(el);
					}

				for (auto& c : sr.albums) {
					auto* el = doc.NewElement("album");
					el->SetAttribute("id",     c.id);
					el->SetAttribute("parent", c.parent_id);
					el->SetAttribute("isDir",  true);
					el->SetAttribute("title",  c.title.c_str());
					el->SetAttribute("artist", c.artist.c_str());
					el->SetAttribute("album",  c.title.c_str());
					if (c.cover_art_id >= 0) el->SetAttribute("coverArt", c.cover_art_id);
					result->InsertEndChild(el);
					}

				for (auto& s : sr.songs)
					result->InsertEndChild(song_entry_xml(doc, s, "song", mbr));

				for (auto& h : sr.chapters) {
					auto* el = doc.NewElement("chapter");
					el->SetAttribute("songId", h.song_id);
					el->SetAttribute("parent", h.parent_id);
					el->SetAttribute("index",  h.index);
					char buf[32];
					std::snprintf(buf, sizeof buf, "%.3f",
					              std::llround(h.start * 1000.0) / 1000.0);
					el->SetAttribute("start",  buf);
					el->SetAttribute("name",   h.name.c_str());
					el->SetAttribute("track",  h.track.c_str());
					el->SetAttribute("album",  h.album.c_str());
					el->SetAttribute("artist", h.artist.c_str());
					result->InsertEndChild(el);
					}

				root->InsertEndChild(result);
				});
		res.set_content(body, use_json ? "application/json" : "application/xml");
		};
	server_.Get("/rest/search2.view", [search_handler](const httplib::Request& req,
	                                                    httplib::Response& res) {
		search_handler(req, res, "searchResult2");
		});
	server_.Get("/rest/search3.view", [search_handler](const httplib::Request& req,
	                                                    httplib::Response& res) {
		search_handler(req, res, "searchResult3");
		});
	}
