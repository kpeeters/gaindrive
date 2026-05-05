#include "gaindrive.hh"
#include "stamp.hh"
#include "streamer.hh"
#include "embedded_web.hh"

#include <chrono>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <memory>
#include <thread>

#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>

#include <reproc++/reproc.hpp>

#include <tinyxml2.h>
#include <nlohmann/json.hpp>
#include <taglib/fileref.h>
#include <taglib/tag.h>
#include <tpropertymap.h>

using namespace tinyxml2;

// ---- Subsonic XML helpers ---------------------------------------------

static const char* SUBSONIC_NS  = "http://subsonic.org/restapi";
static const char* SUBSONIC_VER = "1.16.1";

// Creates a <subsonic-response> root element inside doc and returns it.
static XMLElement* make_root(XMLDocument& doc, const char* status)
	{
	doc.InsertEndChild(doc.NewDeclaration());
	auto* root = doc.NewElement("subsonic-response");
	root->SetAttribute("xmlns",        SUBSONIC_NS);
	root->SetAttribute("status",       status);
	root->SetAttribute("version",      SUBSONIC_VER);
	root->SetAttribute("openSubsonic", "true");
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
	r["status"]       = "ok";
	r["version"]      = SUBSONIC_VER;
	r["openSubsonic"] = true;
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
	j["subsonic-response"]["openSubsonic"]     = true;
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

// Mirrors the bitrate-cap branch of Streamer::serve(): when the source bitrate
// exceeds the user's max_bitrate, stream.view transcodes to mp3 at max_bitrate.
// Returns true and fills the out-params when a transcode would happen, false
// when the source is served as-is.  Format-override (the `format` query param
// on stream) is not modelled here — the gaindrive web client does not pass it,
// and the transcoded* fields are conventionally a per-user property in
// Subsonic, not per-request.
static bool transcode_target(const MediaStore::ChildEntry& c, int max_bitrate,
                              const char*& mime, const char*& suffix,
                              int& bitrate)
	{
	if (max_bitrate <= 0 || c.bitrate <= 0 || c.bitrate <= max_bitrate)
		return false;
	mime    = "audio/mpeg";
	suffix  = "mp3";
	bitrate = max_bitrate;
	return true;
	}

// Serialises a song ChildEntry into a JSON object.  When max_bitrate causes a
// transcode, also emits transcodedContentType / transcodedSuffix (standard
// Subsonic) and transcodedBitRate (gaindrive extension; ignored by clients
// that don't know it) so the client knows the actual stream format.
static nlohmann::json song_entry_json(const MediaStore::ChildEntry& c,
                                       int max_bitrate = 0)
	{
	nlohmann::json s = {
		{"id",          c.id},
		{"parent",      c.parent_id},
		{"isDir",       false},
		{"title",       c.title},
		{"artist",      c.artist},
		{"album",       c.album},
		{"track",       c.track_number},
		{"discNumber",  c.disc_number},
		{"year",        c.year},
		{"genre",       c.genre},
		{"size",        c.file_size},
		{"contentType", codec_to_mime(c.codec)},
		{"suffix",      c.codec},
		{"duration",    (int)c.duration},
		{"bitRate",     c.bitrate}
		};
	if (c.cover_art_id >= 0) s["coverArt"] = c.cover_art_id;
	if (c.starred) s["starred"] = true;
	const char* tmime; const char* tsuffix; int tbitrate;
	if (transcode_target(c, max_bitrate, tmime, tsuffix, tbitrate)) {
		s["transcodedContentType"] = tmime;
		s["transcodedSuffix"]      = tsuffix;
		s["transcodedBitRate"]     = tbitrate;
		}
	return s;
	}

// Creates an XML element for a song with the given tag name.
static XMLElement* song_entry_xml(XMLDocument& doc,
                                   const MediaStore::ChildEntry& c,
                                   const char* tag,
                                   int max_bitrate = 0)
	{
	auto* el = doc.NewElement(tag);
	el->SetAttribute("id",          c.id);
	el->SetAttribute("parent",      c.parent_id);
	el->SetAttribute("isDir",       false);
	el->SetAttribute("title",       c.title.c_str());
	el->SetAttribute("artist",      c.artist.c_str());
	el->SetAttribute("album",       c.album.c_str());
	if (c.cover_art_id >= 0) el->SetAttribute("coverArt", c.cover_art_id);
	el->SetAttribute("track",       c.track_number);
	el->SetAttribute("discNumber",  c.disc_number);
	el->SetAttribute("year",        c.year);
	el->SetAttribute("genre",       c.genre.c_str());
	el->SetAttribute("size",        (int64_t)c.file_size);
	el->SetAttribute("contentType", codec_to_mime(c.codec));
	el->SetAttribute("suffix",      c.codec.c_str());
	el->SetAttribute("duration",    (int)c.duration);
	el->SetAttribute("bitRate",     c.bitrate);
	if (c.starred) el->SetAttribute("starred", "true");
	const char* tmime; const char* tsuffix; int tbitrate;
	if (transcode_target(c, max_bitrate, tmime, tsuffix, tbitrate)) {
		el->SetAttribute("transcodedContentType", tmime);
		el->SetAttribute("transcodedSuffix",      tsuffix);
		el->SetAttribute("transcodedBitRate",     tbitrate);
		}
	return el;
	}

// Looks up the authenticated user's max_bitrate so song entries can advertise
// the transcoded* fields.  Must be called only after check_auth has succeeded.
// Returns 0 (= unlimited / no transcode) if the user record can't be read.
static int request_max_bitrate(const httplib::Request& req, MediaStore& store)
	{
	auto it = req.params.find("u");
	if (it == req.params.end()) return 0;
	auto u = store.get_user(it->second);
	return u ? u->max_bitrate : 0;
	}

// Builds the full subsonic response body for a playlist with its songs.
static std::string playlist_body(const MediaStore::PlaylistInfo& pl, bool use_json,
                                  int max_bitrate)
	{
	if (use_json)
		return subsonic_ok_json([&pl, max_bitrate](nlohmann::json& r) {
			nlohmann::json entries = nlohmann::json::array();
			for (auto& c : pl.songs)
				entries.push_back(song_entry_json(c, max_bitrate));
			r["playlist"] = {
				{"id",        pl.id},
				{"name",      pl.name},
				{"comment",   pl.comment},
				{"owner",     pl.owner},
				{"public",    pl.is_public},
				{"songCount", pl.song_count},
				{"duration",  pl.duration},
				{"created",   pl.created},
				{"changed",   pl.updated},
				{"entry",     entries}
				};
			});
	return subsonic_ok([&pl, max_bitrate](XMLDocument& doc, XMLElement* root) {
		auto* playlist = doc.NewElement("playlist");
		playlist->SetAttribute("id",        pl.id);
		playlist->SetAttribute("name",      pl.name.c_str());
		playlist->SetAttribute("comment",   pl.comment.c_str());
		playlist->SetAttribute("owner",     pl.owner.c_str());
		playlist->SetAttribute("public",    pl.is_public);
		playlist->SetAttribute("songCount", pl.song_count);
		playlist->SetAttribute("duration",  pl.duration);
		playlist->SetAttribute("created",   pl.created.c_str());
		playlist->SetAttribute("changed",   pl.updated.c_str());
		for (auto& c : pl.songs)
			playlist->InsertEndChild(song_entry_xml(doc, c, "entry", max_bitrate));
		root->InsertEndChild(playlist);
		});
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
// Returns true if the authenticated user has cast permission.
static bool check_cast_perm(const httplib::Request& req, httplib::Response& res,
                             MediaStore& store, bool use_json)
	{
	auto u    = req.get_param_value("u");
	auto info = store.get_user(u);
	if (!info || !info->cast_allowed) {
		const char* msg = "User is not authorized for the given operation.";
		res.set_content(use_json ? subsonic_error_json(50, msg)
		                        : subsonic_error(50, msg),
		                use_json ? "application/json" : "text/xml");
		return false;
		}
	return true;
	}

// ---- Artist info helper -----------------------------------------------

// Shared implementation for getArtistInfo and getArtistInfo2.
// key is "artistInfo" or "artistInfo2" — controls the XML element / JSON key.
static void handle_artist_info(const httplib::Request& req, httplib::Response& res,
                                MediaStore& store, bool debug, const char* key)
	{
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
	std::string name = store.get_folder_name(id);
	if (name.empty()) { err(70, "Artist not found."); return; }

	auto cached = store.get_cached_artist_info(id);
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

				// Prefer direct wikipedia relation; fall back to wikidata.
				std::string wiki_title;
				for (auto& rel : rels) {
					std::string type     = rel.value("type","");
					std::string resource = rel.value("url", nlohmann::json::object())
					                          .value("resource","");
					if (type == "allmusic" && info.allmusic_url.empty()) {
						info.allmusic_url = resource;
						std::cout << stamp() << "getArtistInfo [" << name
						          << "] AllMusic: " << resource << std::endl;
						}
					else if (type == "wikipedia") {
						auto pos = resource.find("/wiki/");
						if (pos != std::string::npos) {
							wiki_title = resource.substr(pos + 6);
							std::cout << stamp() << "getArtistInfo [" << name
							          << "] Wikipedia (direct): " << wiki_title << std::endl;
							break;
							}
						}
					else if (type == "wikidata" && wiki_title.empty()) {
						auto pos = resource.rfind('/');
						if (pos == std::string::npos) continue;
						std::string entity = resource.substr(pos + 1);
						std::cout << stamp() << "getArtistInfo [" << name
						          << "] Wikidata entity: " << entity << std::endl;
						httplib::SSLClient wd("www.wikidata.org");
						wd.set_default_headers({
							{"User-Agent","GainDrive/0.1 (https://github.com/kpeeters/gaindrive)"}
							});
						auto rwd = wd.Get("/w/api.php",
							httplib::Params{
								{"action","wbgetentities"},{"ids",entity},
								{"props","sitelinks"},{"sitefilter","enwiki"},
								{"format","json"}
								},
							httplib::Headers{});
						if (rwd && rwd->status == 200) {
							auto jwd = nlohmann::json::parse(rwd->body, nullptr, false);
							if (!jwd.is_discarded())
								wiki_title = jwd["entities"][entity]["sitelinks"]["enwiki"]
								                .value("title","");
							if (!wiki_title.empty())
								std::cout << stamp() << "getArtistInfo [" << name
								          << "] Wikipedia (via Wikidata): "
								          << wiki_title << std::endl;
							}
						}
					}

				// Step 3 — Wikipedia REST summary → bio + thumbnail.
				if (!wiki_title.empty()) {
					httplib::SSLClient wp("en.wikipedia.org");
					wp.set_default_headers({
						{"User-Agent","GainDrive/0.1 (https://github.com/kpeeters/gaindrive)"}
						});
					std::string path_title = wiki_title;
					for (char& c : path_title) if (c == ' ') c = '_';
					auto r3 = wp.Get("/api/rest_v1/page/summary/" + path_title,
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
							info.wiki_url  = "https://en.wikipedia.org/wiki/" + wiki_title;
							if (j3.contains("thumbnail"))
								info.image_url = j3["thumbnail"].value("source","");
							std::cout << stamp() << "getArtistInfo [" << name
							          << "] bio=" << info.biography.size()
							          << " chars, image="
							          << (info.image_url.empty() ? "(none)" : info.image_url)
							          << std::endl;
							}
						}
					}
				}
			}

		store.cache_artist_info(id, info);
		std::cout << stamp() << "getArtistInfo [" << name << "] cached"
		          << " mbid=" << (info.mbid.empty() ? "(none)" : info.mbid)
		          << std::endl;
		}

	// Build response. All fields are child elements per the Subsonic spec.
	auto add_text_el = [](XMLDocument& doc, XMLElement* parent,
	                       const char* tag, const std::string& val) {
		if (val.empty()) return;
		auto* el = doc.NewElement(tag);
		el->SetText(val.c_str());
		parent->InsertEndChild(el);
		};

	std::string body;
	if (use_json)
		body = subsonic_ok_json([&info, key](nlohmann::json& r) {
			nlohmann::json ai = nlohmann::json::object();
			if (!info.biography.empty())      ai["biography"]     = info.biography;
			if (!info.mbid.empty())           ai["musicBrainzId"] = info.mbid;
			if (!info.last_fm_url.empty())    ai["lastFmUrl"]     = info.last_fm_url;
			if (!info.wiki_url.empty())       ai["wikiUrl"]       = info.wiki_url;
			if (!info.allmusic_url.empty())   ai["allMusicUrl"]   = info.allmusic_url;
			if (!info.image_url.empty()) {
				ai["smallImageUrl"]  = info.image_url;
				ai["mediumImageUrl"] = info.image_url;
				ai["largeImageUrl"]  = info.image_url;
				}
			r[key] = ai;
			});
	else
		body = subsonic_ok([&info, &add_text_el, key](XMLDocument& doc, XMLElement* root) {
			auto* ai = doc.NewElement(key);
			add_text_el(doc, ai, "biography",     info.biography);
			add_text_el(doc, ai, "musicBrainzId", info.mbid);
			add_text_el(doc, ai, "lastFmUrl",     info.last_fm_url);
			add_text_el(doc, ai, "wikiUrl",       info.wiki_url);
			add_text_el(doc, ai, "allMusicUrl",   info.allmusic_url);
			add_text_el(doc, ai, "smallImageUrl",  info.image_url);
			add_text_el(doc, ai, "mediumImageUrl", info.image_url);
			add_text_el(doc, ai, "largeImageUrl",  info.image_url);
			root->InsertEndChild(ai);
			});
	if (debug) std::cout << body << "\n";
	res.set_content(body, use_json ? "application/json" : "application/xml");
	}

// ---- Album info helper -----------------------------------------------

// Shared implementation for getAlbumInfo and getAlbumInfo2.
// Searches MusicBrainz for the release-group, then resolves a Wikipedia
// article via Wikidata if needed. Results are cached in album_info_cache.
static void handle_album_info(const httplib::Request& req, httplib::Response& res,
                               MediaStore& store, bool debug)
	{
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
	auto album_data = store.get_album(id);
	if (!album_data) { err(70, "Album not found."); return; }

	const std::string& title  = album_data->album.title;
	const std::string& artist = album_data->album.artist;

	auto cached = store.get_cached_album_info(id);
	MediaStore::CachedAlbumInfo info;
	if (cached) {
		info = *cached;
		std::cout << stamp() << "getAlbumInfo [" << title << "] cached"
		          << " mbid=" << (info.mbid.empty() ? "(none)" : info.mbid)
		          << std::endl;
		}
	else {
		std::cout << stamp() << "getAlbumInfo [" << title
		          << "] querying MusicBrainz" << std::endl;
		httplib::SSLClient mb("musicbrainz.org");
		mb.set_default_headers({
			{"User-Agent", "GainDrive/0.1 (https://github.com/kpeeters/gaindrive)"}
			});

		// Step 1 — search for the release-group by title + artist.
		httplib::Params p1{
			{"query", "releasegroup:\"" + title + "\" AND artist:\"" + artist + "\""},
			{"limit", "1"},
			{"fmt",   "json"}
			};
		auto r1 = mb.Get("/ws/2/release-group", p1, httplib::Headers{});
		if (!r1) {
			std::cout << stamp() << "getAlbumInfo [" << title
			          << "] MusicBrainz request failed (no response)" << std::endl;
			}
		else if (r1->status != 200) {
			std::cout << stamp() << "getAlbumInfo [" << title
			          << "] MusicBrainz HTTP " << r1->status << std::endl;
			}
		else {
			auto j1 = nlohmann::json::parse(r1->body, nullptr, false);
			if (!j1.is_discarded() && j1.contains("release-groups")
			                       && !j1["release-groups"].empty())
				info.mbid = j1["release-groups"][0].value("id", "");
			}

		// Step 2 — fetch URL relations for the release-group.
		if (!info.mbid.empty()) {
			std::this_thread::sleep_for(std::chrono::seconds(1));
			auto r2 = mb.Get("/ws/2/release-group/" + info.mbid,
			                 httplib::Params{{"inc","url-rels"},{"fmt","json"}},
			                 httplib::Headers{});
			if (!r2) {
				std::cout << stamp() << "getAlbumInfo [" << title
				          << "] MusicBrainz url-rels request failed" << std::endl;
				}
			else if (r2->status != 200) {
				std::cout << stamp() << "getAlbumInfo [" << title
				          << "] MusicBrainz url-rels HTTP " << r2->status << std::endl;
				}
			else {
				auto j2 = nlohmann::json::parse(r2->body, nullptr, false);
				auto rels = j2.value("relations", nlohmann::json::array());
				std::cout << stamp() << "getAlbumInfo [" << title
				          << "] MusicBrainz url-rels: " << rels.size()
				          << " relation(s)";
				for (auto& rel : rels) std::cout << " [" << rel.value("type","?") << "]";
				std::cout << std::endl;

				// Prefer direct wikipedia relation; fall back to wikidata.
				std::string wiki_title;
				for (auto& rel : rels) {
					std::string type     = rel.value("type","");
					std::string resource = rel.value("url", nlohmann::json::object())
					                          .value("resource","");
					if (type == "allmusic" && info.allmusic_url.empty()) {
						info.allmusic_url = resource;
						std::cout << stamp() << "getAlbumInfo [" << title
						          << "] AllMusic: " << resource << std::endl;
						}
					else if (type == "wikipedia") {
						auto pos = resource.find("/wiki/");
						if (pos != std::string::npos) {
							wiki_title = resource.substr(pos + 6);
							std::cout << stamp() << "getAlbumInfo [" << title
							          << "] Wikipedia (direct): " << wiki_title
							          << std::endl;
							break;
							}
						}
					else if (type == "wikidata" && wiki_title.empty()) {
						auto pos = resource.rfind('/');
						if (pos == std::string::npos) continue;
						std::string entity = resource.substr(pos + 1);
						std::cout << stamp() << "getAlbumInfo [" << title
						          << "] Wikidata entity: " << entity << std::endl;
						httplib::SSLClient wd("www.wikidata.org");
						wd.set_default_headers({
							{"User-Agent","GainDrive/0.1 (https://github.com/kpeeters/gaindrive)"}
							});
						auto rwd = wd.Get("/w/api.php",
							httplib::Params{
								{"action","wbgetentities"},{"ids",entity},
								{"props","sitelinks"},{"sitefilter","enwiki"},
								{"format","json"}
								},
							httplib::Headers{});
						if (rwd && rwd->status == 200) {
							auto jwd = nlohmann::json::parse(rwd->body, nullptr, false);
							if (!jwd.is_discarded())
								wiki_title = jwd["entities"][entity]["sitelinks"]["enwiki"]
								                .value("title","");
							if (!wiki_title.empty())
								std::cout << stamp() << "getAlbumInfo [" << title
								          << "] Wikipedia (via Wikidata): "
								          << wiki_title << std::endl;
							}
						}
					}

				// Step 3 — Wikipedia REST summary → notes text.
				if (!wiki_title.empty()) {
					httplib::SSLClient wp("en.wikipedia.org");
					wp.set_default_headers({
						{"User-Agent","GainDrive/0.1 (https://github.com/kpeeters/gaindrive)"}
						});
					std::string path_title = wiki_title;
					for (char& c : path_title) if (c == ' ') c = '_';
					auto r3 = wp.Get("/api/rest_v1/page/summary/" + path_title,
					                 httplib::Params{}, httplib::Headers{});
					if (!r3) {
						std::cout << stamp() << "getAlbumInfo [" << title
						          << "] Wikipedia request failed" << std::endl;
						}
					else if (r3->status != 200) {
						std::cout << stamp() << "getAlbumInfo [" << title
						          << "] Wikipedia HTTP " << r3->status << std::endl;
						}
					else {
						auto j3 = nlohmann::json::parse(r3->body, nullptr, false);
						if (!j3.is_discarded()) {
							info.notes    = j3.value("extract", "");
							info.wiki_url = "https://en.wikipedia.org/wiki/" + wiki_title;
							std::cout << stamp() << "getAlbumInfo [" << title
							          << "] notes=" << info.notes.size()
							          << " chars" << std::endl;
							}
						}
					}
				}
			}

		store.cache_album_info(id, info);
		std::cout << stamp() << "getAlbumInfo [" << title << "] cached"
		          << " mbid=" << (info.mbid.empty() ? "(none)" : info.mbid)
		          << std::endl;
		}

	std::string body;
	if (use_json)
		body = subsonic_ok_json([&info](nlohmann::json& r) {
			nlohmann::json ai = nlohmann::json::object();
			if (!info.mbid.empty())           ai["musicBrainzId"] = info.mbid;
			if (!info.notes.empty())          ai["notes"]         = info.notes;
			if (!info.wiki_url.empty())       ai["wikiUrl"]       = info.wiki_url;
			if (!info.allmusic_url.empty())   ai["allMusicUrl"]   = info.allmusic_url;
			r["albumInfo2"] = ai;
			});
	else {
		auto add_text_el = [](XMLDocument& doc, XMLElement* parent,
		                       const char* tag, const std::string& val) {
			if (val.empty()) return;
			auto* el = doc.NewElement(tag);
			el->SetText(val.c_str());
			parent->InsertEndChild(el);
			};
		body = subsonic_ok([&info, &add_text_el](XMLDocument& doc, XMLElement* root) {
			auto* ai = doc.NewElement("albumInfo2");
			add_text_el(doc, ai, "musicBrainzId", info.mbid);
			add_text_el(doc, ai, "notes",         info.notes);
			add_text_el(doc, ai, "wikiUrl",       info.wiki_url);
			add_text_el(doc, ai, "allMusicUrl",   info.allmusic_url);
			root->InsertEndChild(ai);
			});
		}
	if (debug) std::cout << body << "\n";
	res.set_content(body, use_json ? "application/json" : "application/xml");
	}

// ---- Album list helper -----------------------------------------------

// Shared implementation for getAlbumList and getAlbumList2.
// key is "albumList" or "albumList2".
static void handle_album_list(const httplib::Request& req, httplib::Response& res,
                               MediaStore& store, bool debug, const char* key)
	{
	bool use_json = (fmt_of(req) == "json");

	auto param_int = [&](const char* name, int def) {
		auto it = req.params.find(name);
		return it != req.params.end() ? std::stoi(it->second) : def;
		};
	auto param_str = [&](const char* name) {
		auto it = req.params.find(name);
		return it != req.params.end() ? it->second : std::string{};
		};

	std::string type     = param_str("type");
	if (type.empty()) type = "newest";
	int size             = std::min(500, std::max(1, param_int("size", 10)));
	int offset           = param_int("offset", 0);
	int from_year        = param_int("fromYear", 0);
	int to_year          = param_int("toYear",   0);
	std::string genre    = param_str("genre");
	std::string username = param_str("u");

	auto albums = store.get_album_list(type, size, offset,
	                                   from_year, to_year, genre, username);

	std::string body;
	if (use_json)
		body = subsonic_ok_json([&albums, key](nlohmann::json& r) {
			nlohmann::json arr = nlohmann::json::array();
			for (auto& al : albums) {
				nlohmann::json entry = {
					{"id",        al.id},
					{"parent",    al.parent_id},
					{"isDir",     true},
					{"title",     al.title},
					{"name",      al.title},
					{"artist",    al.artist},
					{"songCount", al.song_count},
					{"duration",  al.duration},
					{"created",   al.created}
					};
				if (al.cover_art_id >= 0) entry["coverArt"] = al.cover_art_id;
				if (al.year > 0)          entry["year"]     = al.year;
				if (!al.genre.empty())    entry["genre"]    = al.genre;
				if (al.starred)           entry["starred"]  = true;
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
				if (!al.created.empty()) el->SetAttribute("created", al.created.c_str());
				if (al.year > 0)         el->SetAttribute("year",    al.year);
				if (!al.genre.empty())   el->SetAttribute("genre",   al.genre.c_str());
				if (al.starred)          el->SetAttribute("starred", "true");
				list->InsertEndChild(el);
				}
			root->InsertEndChild(list);
			});
	if (debug) std::cout << body << "\n";
	res.set_content(body, use_json ? "application/json" : "application/xml");
	}

// ---- GainDrive --------------------------------------------------------

// Grace period before the SSE-as-heartbeat watchdog tears down a cast
// session whose listener has gone away. Long enough to ride out a page
// reload or a brief network blip; short enough that closing the tab
// actually stops the cast.
static constexpr int CAST_IDLE_GRACE_S = 30;

GainDrive::GainDrive(const std::string& db_path,
                     const std::string& music_root,
                     const std::string& upload_dir,
                     bool no_scan,
                     bool debug,
                     bool flat_multi_disc)
	: debug_(debug), flat_multi_disc_(flat_multi_disc), upload_dir_(upload_dir),
	  store_(db_path, music_root), watcher_(store_, music_root)
	{
	namespace fs = std::filesystem;
	if (!fs::exists(upload_dir_))
		fs::create_directories(upload_dir_);

	// Normalise /rest/foo → /rest/foo.view so clients that omit the suffix still work.
	server_.set_pre_routing_handler([](const httplib::Request& req, httplib::Response&) {
		auto& path = const_cast<httplib::Request&>(req).path;
		if (path.rfind("/rest/", 0) == 0 && path.find('.') == std::string::npos)
			path += ".view";
		return httplib::Server::HandlerResponse::Unhandled;
		});

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

	// Audio streams are consumed at playback speed, so the send buffer can stay
	// full for a long time while the Chromecast plays through its local buffer.
	// Increase the write timeout well beyond the longest expected track to prevent
	// httplib from closing the connection mid-stream.
	server_.set_write_timeout(3600, 0);   // 1 hour

	// Enable TCP keepalives so the NAT table entry stays alive while the Cast
	// receiver has its TCP window at zero (buffer full, not reading).  Without
	// this the router drops the idle connection after ~60-90 s.
	// Accepted sockets inherit SO_KEEPALIVE from the listening socket on Linux.
	server_.set_socket_options([](int sock) {
		httplib::default_socket_options(sock);
		int on = 1;
		setsockopt(sock, SOL_SOCKET,  SO_KEEPALIVE,   &on, sizeof(on));
		int idle  = 10;   // start probing after 10 s of silence
		int intvl =  5;   // probe every 5 s
		int cnt   =  3;   // give up after 3 missed probes
		setsockopt(sock, IPPROTO_TCP, TCP_KEEPIDLE,   &idle,  sizeof(idle));
		setsockopt(sock, IPPROTO_TCP, TCP_KEEPINTVL,  &intvl, sizeof(intvl));
		setsockopt(sock, IPPROTO_TCP, TCP_KEEPCNT,    &cnt,   sizeof(cnt));
		});

	// Web client — serve embedded static files.
	server_.Get("/", [](const httplib::Request&, httplib::Response& res) {
		res.set_content(embedded::index_html.data(), embedded::index_html.size(),
		                embedded::index_html_mime.data());
		});
	server_.Get("/index.html", [](const httplib::Request&, httplib::Response& res) {
		res.set_content(embedded::index_html.data(), embedded::index_html.size(),
		                embedded::index_html_mime.data());
		});
	server_.Get("/style.css", [](const httplib::Request&, httplib::Response& res) {
		res.set_content(embedded::style_css.data(), embedded::style_css.size(),
		                embedded::style_css_mime.data());
		});
	server_.Get("/app.js", [](const httplib::Request&, httplib::Response& res) {
		res.set_content(embedded::app_js.data(), embedded::app_js.size(),
		                embedded::app_js_mime.data());
		});
	server_.Get("/favicon.svg", [](const httplib::Request&, httplib::Response& res) {
		res.set_content(embedded::favicon_svg.data(), embedded::favicon_svg.size(),
		                embedded::favicon_svg_mime.data());
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

	// getOpenSubsonicExtensions — no auth required; clients call this before login.
	server_.Get("/rest/getOpenSubsonicExtensions.view",
	            [this](const httplib::Request& req, httplib::Response& res) {
		bool use_json = (fmt_of(req) == "json");
		std::string body;
		if (use_json)
			body = subsonic_ok_json([](nlohmann::json& r) {
				r["openSubsonicExtensions"] = {{{"name", "gaindrive"}, {"versions", {1}}}};
				});
		else
			body = subsonic_ok([](XMLDocument& doc, XMLElement* root) {
				auto* exts = doc.NewElement("openSubsonicExtensions");
				auto* ext  = doc.NewElement("extension");
				ext->SetAttribute("name", "gaindrive");
				ext->SetAttribute("versions", "1");
				exts->InsertEndChild(ext);
				root->InsertEndChild(exts);
				});
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
		if (debug_) std::cout << body << "\n";
		res.set_content(body, use_json ? "application/json" : "application/xml");
		});

	// getUsers — returns all users; admin only.
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
		if (debug_) std::cout << body << "\n";
		res.set_content(body, use_json ? "application/json" : "application/xml");
		});

	// createUser — creates a new user; admin only.
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
		std::string password = qp("password");
		if (username.empty()) { err(10, "Required parameter missing: username."); return; }
		if (password.empty()) { err(10, "Required parameter missing: password."); return; }

		bool is_admin       = (qp("adminRole")  == "true");
		bool upload_allowed = (qp("uploadRole") == "true");
		bool disabled       = (qp("disabled")   == "true");
		bool cast_allowed   = (qp("castRole")   == "true");
		int  max_bitrate    = 0;
		if (!qp("maxBitRate").empty()) max_bitrate = std::stoi(qp("maxBitRate"));

		if (!store_.add_user(username, password, is_admin)) {
			err(0, "User already exists.");
			return;
			}
		store_.update_user(username, "", qp("email"), is_admin, max_bitrate, upload_allowed, disabled, cast_allowed);

		std::string body = use_json ? subsonic_ok_json() : subsonic_ok();
		if (debug_) std::cout << body << "\n";
		res.set_content(body, use_json ? "application/json" : "application/xml");
		});

	// updateUser — updates an existing user; admin only.
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
		int  max_bitrate    = qp("maxBitRate").empty()  ? existing->max_bitrate    : std::stoi(qp("maxBitRate"));
		std::string email   = qp("email").empty()       ? existing->email          : qp("email");
		std::string pw      = qp("password");

		// An admin cannot disable their own account.
		if (username == qp("u") && disabled)
			{ err(0, "You cannot disable your own account."); return; }

		store_.update_user(username, pw, email, is_admin, max_bitrate, upload_allowed, disabled, cast_allowed);

		std::string body = use_json ? subsonic_ok_json() : subsonic_ok();
		if (debug_) std::cout << body << "\n";
		res.set_content(body, use_json ? "application/json" : "application/xml");
		});

	// changePassword — change a user's password; admins can change any user's,
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
		std::string password  = qp("password");
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

	// getArtists — same artists as getIndexes but with albumCount per artist.
	server_.Get("/rest/getArtists.view", [this](const httplib::Request& req,
	                                            httplib::Response& res) {
		if (!check_auth(req, res, store_)) return;

		auto artists = store_.get_artist_dirs();
		std::sort(artists.begin(), artists.end(),
			[](const MediaStore::ArtistDir& a, const MediaStore::ArtistDir& b) {
				return sort_key(a.name) < sort_key(b.name);
				});

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
						artist_arr.push_back({{"id", a->id}, {"name", a->name},
						                      {"albumCount", a->album_count}});
					idx_arr.push_back({{"name", letter}, {"artist", artist_arr}});
					}
				r["artists"] = {
					{"lastModified",    "0"},
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
						idx->InsertEndChild(artist);
						}
					artists_el->InsertEndChild(idx);
					}
				root->InsertEndChild(artists_el);
				});
		if (debug_) std::cout << body << "\n";
		res.set_content(body, use_json ? "application/json" : "application/xml");
		});

	// getArtist — single artist with album list.
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
		auto info = store_.get_artist(std::stoi(it->second), user);
		if (!info) { err(70, "Artist not found."); return; }

		std::string body;
		if (use_json)
			body = subsonic_ok_json([&info](nlohmann::json& r) {
				nlohmann::json arr = nlohmann::json::array();
				for (auto& al : info->albums) {
					nlohmann::json entry = {
						{"id",        al.id},
						{"parent",    al.parent_id},
						{"isDir",     true},
						{"title",     al.title},
						{"name",      al.title},
						{"artist",    al.artist},
						{"songCount", al.song_count},
						{"duration",  al.duration},
						{"created",   al.created}
						};
					if (al.cover_art_id >= 0) entry["coverArt"] = al.cover_art_id;
					if (al.year > 0)          entry["year"]     = al.year;
					if (!al.genre.empty())    entry["genre"]    = al.genre;
					if (al.starred)           entry["starred"]  = true;
					arr.push_back(std::move(entry));
					}
				r["artist"] = {
					{"id",         info->artist.id},
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
					if (!al.created.empty()) el->SetAttribute("created", al.created.c_str());
					if (al.year > 0)         el->SetAttribute("year",    al.year);
					if (!al.genre.empty())   el->SetAttribute("genre",   al.genre.c_str());
					if (al.starred)          el->SetAttribute("starred", "true");
					artist_el->InsertEndChild(el);
					}
				root->InsertEndChild(artist_el);
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

		auto dir = store_.get_directory(std::stoi(it->second), flat_multi_disc_);
		if (!dir) { err(70, "Directory not found."); return; }

		int mbr = request_max_bitrate(req, store_);
		std::string body;
		if (use_json)
			body = subsonic_ok_json([&dir, mbr](nlohmann::json& r) {
				nlohmann::json children = nlohmann::json::array();
				for (auto& c : dir->children) {
					nlohmann::json child;
					if (c.is_dir) {
						child = {{"id",c.id},{"parent",c.parent_id},{"isDir",true},
						         {"title",c.title},{"artist",c.artist},{"album",c.album}};
						if (c.cover_art_id >= 0) child["coverArt"] = c.cover_art_id;
						if (c.year > 0)          child["year"]     = c.year;
						} else {
						child = song_entry_json(c, mbr);
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
		if (debug_) std::cout << body << "\n";
		res.set_content(body, use_json ? "application/json" : "application/xml");
		});

	// getAlbumList / getAlbumList2 — both use the same folder-based logic.
	server_.Get("/rest/getAlbumList.view", [this](const httplib::Request& req,
	                                              httplib::Response& res) {
		if (!check_auth(req, res, store_)) return;
		handle_album_list(req, res, store_, debug_, "albumList");
		});
	server_.Get("/rest/getAlbumList2.view", [this](const httplib::Request& req,
	                                               httplib::Response& res) {
		if (!check_auth(req, res, store_)) return;
		handle_album_list(req, res, store_, debug_, "albumList2");
		});

	// getArtistInfo / getArtistInfo2 — MusicBrainz lookup, result cached in DB.
	// Both endpoints share identical logic; only the response key name differs.
	server_.Get("/rest/getArtistInfo.view", [this](const httplib::Request& req,
	                                               httplib::Response& res) {
		if (!check_auth(req, res, store_)) return;
		handle_artist_info(req, res, store_, debug_, "artistInfo");
		});
	server_.Get("/rest/getArtistInfo2.view", [this](const httplib::Request& req,
	                                                httplib::Response& res) {
		if (!check_auth(req, res, store_)) return;
		handle_artist_info(req, res, store_, debug_, "artistInfo2");
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

		int folder_id = std::stoi(it->second);
		std::string rel_path = store_.get_cover_path(folder_id);
		if (rel_path.empty()) {
			res.status = 404;
			return;
			}

		// Optional index: 0 (default) = main cover, 1+ = extra images sorted.
		auto idx_it = req.params.find("index");
		if (idx_it != req.params.end()) {
			int idx = std::stoi(idx_it->second);
			if (idx > 0) {
				auto extras = store_.get_extra_image_paths(folder_id);
				if (idx - 1 >= static_cast<int>(extras.size())) {
					res.status = 404;
					return;
					}
				rel_path = extras[idx - 1];
				}
			}

		// Compose absolute filesystem path for the actual file open.
		std::string path = store_.abs_path(rel_path);
		if (!store_.path_is_within_root(path)) {
			std::cout << stamp() << "getCoverArt: refusing path outside music_root: "
			          << path << std::endl;
			res.status = 403;
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

	// getAlbumTexts — list .txt files in an album folder.
	server_.Get("/rest/getAlbumTexts.view", [this](const httplib::Request& req,
	                                               httplib::Response& res) {
		if (!check_auth(req, res, store_)) return;

		auto it = req.params.find("id");
		if (it == req.params.end()) {
			res.set_content(subsonic_error_json(10, "Required parameter missing: id."),
			                "application/json");
			return;
			}

		std::string folder_rel = store_.get_folder_path(std::stoi(it->second));
		if (folder_rel.empty()) {
			res.status = 404;
			return;
			}
		std::string folder = store_.abs_path(folder_rel);
		if (!store_.path_is_within_root(folder)) {
			std::cout << stamp() << "getAlbumTexts: refusing path outside music_root: "
			          << folder << std::endl;
			res.status = 403;
			return;
			}

		// Well-known utility files that are not human-readable liner notes.
		static const std::set<std::string> excluded = {
			"fingerprints.txt",
			};

		namespace fs = std::filesystem;
		nlohmann::json files = nlohmann::json::array();
		try {
			for (auto& entry : fs::directory_iterator(folder)) {
				if (!entry.is_regular_file() || entry.path().extension() != ".txt") continue;
				if (excluded.count(entry.path().filename().string())) continue;
				files.push_back({{"name", entry.path().filename().string()}});
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

	// getAlbumImages — return total image count for an album folder (cover + extras).
	server_.Get("/rest/getAlbumImages.view", [this](const httplib::Request& req,
	                                                httplib::Response& res) {
		if (!check_auth(req, res, store_)) return;

		auto it = req.params.find("id");
		if (it == req.params.end()) {
			res.set_content(subsonic_error_json(10, "Required parameter missing: id."),
			                "application/json");
			return;
			}

		int count = store_.get_image_count(std::stoi(it->second));
		res.set_content(subsonic_ok_json([count](nlohmann::json& r) {
			r["albumImages"] = {{"count", count}};
			}), "application/json");
		});

	// getAlbumText — serve a single .txt file from an album folder.
	server_.Get("/rest/getAlbumText.view", [this](const httplib::Request& req,
	                                              httplib::Response& res) {
		if (!check_auth(req, res, store_)) return;

		auto id_it   = req.params.find("id");
		auto name_it = req.params.find("name");
		if (id_it == req.params.end() || name_it == req.params.end()) {
			res.status = 400;
			return;
			}

		// Reject any path traversal attempts.
		const std::string& name = name_it->second;
		if (name.find('/') != std::string::npos  ||
		    name.find('\\') != std::string::npos ||
		    name.find("..") != std::string::npos ||
		    name.size() < 5 ||
		    name.substr(name.size() - 4) != ".txt") {
			res.status = 400;
			return;
			}

		std::string folder_rel = store_.get_folder_path(std::stoi(id_it->second));
		if (folder_rel.empty()) {
			res.status = 404;
			return;
			}

		namespace fs = std::filesystem;
		fs::path full = fs::path(store_.abs_path(folder_rel)) / name;
		if (!store_.path_is_within_root(full)) {
			std::cout << stamp() << "getAlbumText: refusing path outside music_root: "
			          << full.string() << std::endl;
			res.status = 403;
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

	// savePlayQueue — persist the client's current queue and playback position.
	server_.Get("/rest/savePlayQueue.view", [this](const httplib::Request& req,
	                                               httplib::Response& res) {
		if (!check_auth(req, res, store_)) return;

		auto qp = [&](const std::string& k, const std::string& def = "") {
			auto it = req.params.find(k);
			return it != req.params.end() ? it->second : def;
			};

		// Collect all song IDs — the parameter may be repeated. Resolve
		// each to its filesystem path (the durable key the user-state DB
		// stores). Unresolvable ids are dropped silently.
		std::vector<std::string> paths;
		auto range = req.params.equal_range("id");
		for (auto it = range.first; it != range.second; ++it) {
			if (auto p = store_.song_path_by_id(std::stoi(it->second)))
				paths.push_back(*p);
			}

		int         current_id   = std::stoi(qp("current", "0"));
		std::string current_path = "";
		if (current_id != 0) {
			if (auto p = store_.song_path_by_id(current_id)) current_path = *p;
			}
		int64_t offset_ms  = std::stoll(qp("position", "0"));
		std::string client = qp("c");
		std::string user   = qp("u");

		store_.save_play_queue(user, paths, current_path, offset_ms, client);
		bool use_json = (fmt_of(req) == "json");
		std::string body = use_json ? subsonic_ok_json() : subsonic_ok();
		if (debug_) std::cout << body << "\n";
		res.set_content(body, use_json ? "application/json" : "application/xml");
		});

	// getPlayQueue — retrieve the user's saved play queue and position.
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
					{"changed",    pq->changed},
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
						el->SetAttribute("changed",   pq->changed.c_str());
					if (!pq->client.empty())
						el->SetAttribute("changedBy", pq->client.c_str());
					for (auto& s : pq->songs)
						el->InsertEndChild(song_entry_xml(doc, s, "entry", mbr));
					}
				root->InsertEndChild(el);
				});
		if (debug_) std::cout << body << "\n";
		res.set_content(body, use_json ? "application/json" : "application/xml");
		});

	// createBookmark — mark a playback position within a song.
	// scrobble — record a play (submission=true) or now-playing event (submission=false).
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

		auto range = req.params.equal_range("id");
		for (auto it = range.first; it != range.second; ++it) {
			if (auto p = store_.song_path_by_id(std::stoi(it->second)))
				store_.scrobble(user, *p, submission, client);
			}

		bool use_json = (fmt_of(req) == "json");
		std::string body = use_json ? subsonic_ok_json() : subsonic_ok();
		if (debug_) std::cout << body << "\n";
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

		int     song_id     = std::stoi(it->second);
		int64_t position_ms = std::stoll(qp("position", "0"));
		std::string comment = qp("comment");
		std::string user    = qp("u");

		auto song_path = store_.song_path_by_id(song_id);
		if (!song_path) {
			res.set_content(subsonic_error(70, "Song not found."), "application/xml");
			return;
			}
		store_.create_bookmark(user, *song_path, position_ms, comment);
		bool use_json = (fmt_of(req) == "json");
		std::string body = use_json ? subsonic_ok_json() : subsonic_ok();
		if (debug_) std::cout << body << "\n";
		res.set_content(body, use_json ? "application/json" : "application/xml");
		});

	// stream — serve audio file directly or transcode via ffmpeg.
	// In cast mode: redirect playback to the Chromecast and return 204 to the
	// calling client.  The Chromecast authenticates its own request with a
	// castToken query parameter instead of normal credentials.
	server_.Get("/rest/stream.view", [this](const httplib::Request& req,
	                                        httplib::Response& res) {
		auto tok_it = req.params.find("castToken");
		bool cast_authed = tok_it != req.params.end()
		                && cast_manager_.valid_token(tok_it->second);
		if (!cast_authed && !check_auth(req, res, store_)) return;

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

		// Compose-and-validate the absolute song path once. Streamer reads from
		// it (via std::ifstream and ffmpeg argv) — refuse anything outside
		// music_root before handing it off.
		std::string song_abs = store_.abs_path(song->path);
		if (!store_.path_is_within_root(song_abs)) {
			std::cout << stamp() << "stream: refusing path outside music_root: "
			          << song_abs << std::endl;
			res.status = 403;
			return;
			}

		// If cast mode is active and the caller is not the Chromecast itself,
		// instruct the Chromecast to fetch the stream and return 204 here.
		if (cast_manager_.active() && !cast_authed) {
			std::string host = req.get_header_value("Host");
			if (host.empty()) host = "localhost";
			std::string proto = req.get_header_value("X-Forwarded-Proto");
			if (proto.empty()) proto = "http";
			std::string url = proto + "://" + host + "/rest/stream.view"
			                + "?id=" + it->second
			                + "&castToken=" + cast_manager_.token();
			// Native seek: the URL serves the full file, and the LOAD message
			// tells the receiver where to seek.  No timeOffset in the URL.
			auto to_it = req.params.find("timeOffset");
			float cast_offset = 0.0f;
			if (to_it != req.params.end() && !to_it->second.empty())
				cast_offset = std::stof(to_it->second);
			cast_manager_.load(url, codec_to_mime(song->codec), cast_offset, song->duration);
			res.status = 204;
			return;
			}

		auto qp = [&](const std::string& k, const std::string& def = "") {
			auto it2 = req.params.find(k);
			return it2 != req.params.end() ? it2->second : def;
			};

		int         max_bitrate = std::stoi(qp("maxBitRate", "0"));
		std::string format      = qp("format");
		// The Chromecast sometimes probes the stream URL with timeOffset stripped.
		// Always use the authoritative offset stored at castLoad time for cast
		// requests so the probe and the real request both start at the right position.
		int         time_offset = cast_authed
		    ? static_cast<int>(last_cast_offset_)
		    : std::stoi(qp("timeOffset", "0"));

		// Chromecast metadata probes arrive as Range requests against the stream URL.
		// For seeked streams (time_offset > 0) these would otherwise hit serve_transcoded
		// which returns chunked output with no Content-Length, making it impossible for
		// the receiver to read the format's duration/seek tables.  Force time_offset=0
		// so these Range requests are served directly from the raw file.
		if (cast_authed && !req.get_header_value("Range").empty() && time_offset > 0)
			time_offset = 0;

		// Seeked-stream probe: the Chromecast strips timeOffset from its probe
		// request.  We must not return a full transcoded stream (blasts megabytes
		// at LAN speed while throttle is suppressed during BUFFERING), but we also
		// must not return an empty body (Content-Length: 0 makes the receiver treat
		// the track as finished and abort the real seeked request).  Serve a small
		// slice of the raw file from t=0 — enough for the receiver to validate the
		// URL, well within the pre-buffer window (prebuf ≈ 30 s of audio).
		if (cast_authed
		        && req.params.find("timeOffset") == req.params.end()
		        && last_cast_offset_ > 0.0f
		        && req.get_header_value("Range").empty()) {
			std::cout << stamp() << "cast probe: id=" << it->second
			          << " offset=" << last_cast_offset_ << std::endl;
			Streamer::SongInfo probe_si{ song_abs, song->codec,
			                             song->bitrate, song->duration,
			                             std::min(song->file_size, (int64_t)32768) };
			Streamer::serve(req, res, probe_si, 0, "", 0, true, {});
			return;
			}

		if (cast_authed) {
			// Log Range header so we can see what the Cast receiver is requesting.
			auto range = req.get_header_value("Range");
			std::cout << stamp() << "cast stream: id=" << it->second
			          << " size=" << song->file_size
			          << " range=[" << (range.empty() ? "none" : range) << "]"
			          << std::endl;
			}

		Streamer::SongInfo si{ song_abs, song->codec,
		                       song->bitrate, song->duration, song->file_size };

		// For Cast streams, pass a callback that returns the receiver's current
		// playback position from the cached status (updated every ~0.5 s by the
		// web client's poll).  The streamer uses this to keep the buffer at a
		// stable level without relying on any device-specific buffer size.
		std::function<float()> get_pos;
		if (cast_authed) {
			int gen = cast_manager_.load_generation();
			get_pos = [this, gen]{
				// Return -2 when a newer stream has started — the streamer treats
				// this as a stop signal so the old thread exits promptly.
				if (cast_manager_.load_generation() != gen) return CAST_POS_STOP;
				auto s = cast_manager_.get_status();
				// BUFFERING means "seeking to this position", not "played up to here".
				// Return CAST_POS_BUFFERING to suppress throttle until playback starts.
				if (s.player_state != "PLAYING") return CAST_POS_BUFFERING;
				return s.current_time;
				};
			}

		Streamer::serve(req, res, si, max_bitrate, format, time_offset,
		                cast_authed, std::move(get_pos));
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
		if (it == req.params.end() || it->second.empty()) {
			err(10, "Required parameter missing: name.");
			return;
			}
		std::string name = it->second;
		std::string user = req.params.find("u")->second;

		std::vector<std::string> song_paths;
		auto range = req.params.equal_range("songId");
		for (auto i = range.first; i != range.second; ++i) {
			if (auto p = store_.song_path_by_id(std::stoi(i->second)))
				song_paths.push_back(*p);
			}

		auto pl = store_.create_playlist(user, name, song_paths);
		std::string body = playlist_body(pl, use_json, request_max_bitrate(req, store_));
		if (debug_) std::cout << body << "\n";
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
		auto pl = store_.get_playlist(std::stoi(it->second), user);
		if (!pl) { err(70, "Playlist not found."); return; }

		std::string body = playlist_body(*pl, use_json, request_max_bitrate(req, store_));
		if (debug_) std::cout << body << "\n";
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
						{"id",        pl.id},
						{"name",      pl.name},
						{"comment",   pl.comment},
						{"owner",     pl.owner},
						{"public",    pl.is_public},
						{"songCount", pl.song_count},
						{"duration",  pl.duration},
						{"created",   pl.created},
						{"changed",   pl.updated}
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
					el->SetAttribute("created",   pl.created.c_str());
					el->SetAttribute("changed",   pl.updated.c_str());
					playlists->InsertEndChild(el);
					}
				root->InsertEndChild(playlists);
				});
		if (debug_) std::cout << body << "\n";
		res.set_content(body, use_json ? "application/json" : "application/xml");
		});

	// getStarred / getStarred2 — identical content, only the response key differs.
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
					artists.push_back({{"id", a.id}, {"name", a.name}});

				nlohmann::json albums = nlohmann::json::array();
				for (auto& c : sr.albums) {
					nlohmann::json al = {
						{"id",     c.id},
						{"parent", c.parent_id},
						{"isDir",  true},
						{"title",  c.title},
						{"artist", c.artist},
						{"album",  c.album}
						};
					if (c.cover_art_id >= 0) al["coverArt"] = c.cover_art_id;
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
		if (debug_) std::cout << body << "\n";
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
		int playlist_id = std::stoi(it->second);
		std::string user = req.params.find("u")->second;

		std::optional<std::string> name, comment;
		std::optional<bool> is_public;
		if (req.params.count("name"))    name      = req.params.find("name")->second;
		if (req.params.count("comment")) comment   = req.params.find("comment")->second;
		if (req.params.count("public"))  is_public = (req.params.find("public")->second == "true");

		std::vector<std::string> paths_to_add;
		std::vector<int> to_remove;
		for (auto& [k, v] : req.params) {
			if (k == "songIdToAdd") {
				if (auto p = store_.song_path_by_id(std::stoi(v)))
					paths_to_add.push_back(*p);
				}
			else if (k == "songIndexToRemove") to_remove.push_back(std::stoi(v));
			}

		if (!store_.update_playlist(playlist_id, user, name, comment, is_public,
		                             paths_to_add, to_remove)) {
			err(70, "Playlist not found.");
			return;
			}

		std::string body = use_json ? subsonic_ok_json() : subsonic_ok();
		if (debug_) std::cout << body << "\n";
		res.set_content(body, use_json ? "application/json" : "application/xml");
		});

	// star
	server_.Get("/rest/star.view", [this](const httplib::Request& req,
	                                      httplib::Response& res) {
		if (!check_auth(req, res, store_)) return;
		bool use_json = (fmt_of(req) == "json");
		std::string user = req.params.find("u")->second;

		for (auto& [k, v] : req.params) {
			if (k == "id") {
				if (auto p = store_.song_path_by_id(std::stoi(v)))
					store_.add_star(user, *p, "", "");
				}
			else if (k == "albumId") {
				if (auto p = store_.album_folder_path_by_id(std::stoi(v)))
					store_.add_star(user, "", *p, "");
				}
			else if (k == "artistId") {
				if (auto p = store_.artist_folder_path_by_id(std::stoi(v)))
					store_.add_star(user, "", "", *p);
				}
			}

		std::string body = use_json ? subsonic_ok_json() : subsonic_ok();
		if (debug_) std::cout << body << "\n";
		res.set_content(body, use_json ? "application/json" : "application/xml");
		});

	// unstar
	server_.Get("/rest/unstar.view", [this](const httplib::Request& req,
	                                        httplib::Response& res) {
		if (!check_auth(req, res, store_)) return;
		bool use_json = (fmt_of(req) == "json");
		std::string user = req.params.find("u")->second;

		for (auto& [k, v] : req.params) {
			if (k == "id") {
				if (auto p = store_.song_path_by_id(std::stoi(v)))
					store_.remove_star(user, *p, "", "");
				}
			else if (k == "albumId") {
				if (auto p = store_.album_folder_path_by_id(std::stoi(v)))
					store_.remove_star(user, "", *p, "");
				}
			else if (k == "artistId") {
				if (auto p = store_.artist_folder_path_by_id(std::stoi(v)))
					store_.remove_star(user, "", "", *p);
				}
			}

		std::string body = use_json ? subsonic_ok_json() : subsonic_ok();
		if (debug_) std::cout << body << "\n";
		res.set_content(body, use_json ? "application/json" : "application/xml");
		});

	// getAlbum — single album with its track list.
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
		auto info = store_.get_album(std::stoi(it->second), flat_multi_disc_, user);
		if (!info) { err(70, "Album not found."); return; }
		int mbr = request_max_bitrate(req, store_);

		std::string body;
		if (use_json)
			body = subsonic_ok_json([&info, mbr](nlohmann::json& r) {
				nlohmann::json songs = nlohmann::json::array();
				for (auto& s : info->songs)
					songs.push_back(song_entry_json(s, mbr));
				auto& al = info->album;
				nlohmann::json entry = {
					{"id",        al.id},
					{"parent",    al.parent_id},
					{"name",      al.title},
					{"artist",    al.artist},
					{"songCount", al.song_count},
					{"duration",  al.duration},
					{"created",   al.created},
					{"song",      songs}
					};
				if (al.cover_art_id >= 0) entry["coverArt"] = al.cover_art_id;
				if (al.year > 0)          entry["year"]     = al.year;
				if (!al.genre.empty())    entry["genre"]    = al.genre;
				if (al.starred)           entry["starred"]  = true;
				r["album"] = std::move(entry);
				});
		else
			body = subsonic_ok([&info, mbr](XMLDocument& doc, XMLElement* root) {
				auto& al = info->album;
				auto* el = doc.NewElement("album");
				el->SetAttribute("id",        al.id);
				el->SetAttribute("parent",    al.parent_id);
				el->SetAttribute("name",      al.title.c_str());
				el->SetAttribute("artist",    al.artist.c_str());
				el->SetAttribute("songCount", al.song_count);
				el->SetAttribute("duration",  al.duration);
				if (al.cover_art_id >= 0) el->SetAttribute("coverArt", al.cover_art_id);
				if (!al.created.empty())  el->SetAttribute("created",  al.created.c_str());
				if (al.year > 0)          el->SetAttribute("year",     al.year);
				if (!al.genre.empty())    el->SetAttribute("genre",    al.genre.c_str());
				if (al.starred)           el->SetAttribute("starred",  "true");
				for (auto& s : info->songs)
					el->InsertEndChild(song_entry_xml(doc, s, "song", mbr));
				root->InsertEndChild(el);
				});
		if (debug_) std::cout << body << "\n";
		res.set_content(body, use_json ? "application/json" : "application/xml");
		});

	server_.Get("/rest/getAlbumInfo2.view", [this](const httplib::Request& req,
	                                               httplib::Response& res) {
		if (!check_auth(req, res, store_)) return;
		handle_album_info(req, res, store_, debug_);
		});

	// getTopSongs — play-count tracking not implemented; return empty list.
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
		if (debug_) std::cout << body << "\n";
		res.set_content(body, use_json ? "application/json" : "application/xml");
		});

	// getSong — full metadata for a single track.
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

		auto song = store_.get_song_entry(std::stoi(it->second));
		if (!song) { err(70, "Song not found."); return; }
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
		if (debug_) std::cout << body << "\n";
		res.set_content(body, use_json ? "application/json" : "application/xml");
		});

	// search2 / search3 — title/name substring search across artists, albums, songs.
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

		int artist_count  = std::stoi(qp("artistCount",  "20"));
		int artist_offset = std::stoi(qp("artistOffset", "0"));
		int album_count   = std::stoi(qp("albumCount",   "20"));
		int album_offset  = std::stoi(qp("albumOffset",  "0"));
		int song_count    = std::stoi(qp("songCount",    "20"));
		int song_offset   = std::stoi(qp("songOffset",   "0"));

		auto sr = store_.search(query,
		                        artist_count, artist_offset,
		                        album_count,  album_offset,
		                        song_count,   song_offset);
		int mbr = request_max_bitrate(req, store_);

		std::string body;
		if (use_json)
			body = subsonic_ok_json([&sr, key, mbr](nlohmann::json& r) {
				nlohmann::json artists = nlohmann::json::array();
				for (auto& a : sr.artists)
					artists.push_back({{"id", a.id}, {"name", a.title}});

				nlohmann::json albums = nlohmann::json::array();
				for (auto& c : sr.albums) {
					nlohmann::json al = {
						{"id",     c.id},
						{"parent", c.parent_id},
						{"isDir",  true},
						{"title",  c.title},
						{"artist", c.artist},
						{"album",  c.title}
						};
					if (c.cover_art_id >= 0) al["coverArt"] = c.cover_art_id;
					albums.push_back(al);
					}

				nlohmann::json songs = nlohmann::json::array();
				for (auto& s : sr.songs)
					songs.push_back(song_entry_json(s, mbr));

				r[key] = {{"artist", artists}, {"album", albums}, {"song", songs}};
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

				root->InsertEndChild(result);
				});
		if (debug_) std::cout << body << "\n";
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

	// getBookmarks — list all bookmarks for the authenticated user.
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
						{"created",  bm.created},
						{"changed",  bm.changed},
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
					b->SetAttribute("created",  bm.created.c_str());
					b->SetAttribute("changed",  bm.changed.c_str());
					b->InsertEndChild(song_entry_xml(doc, bm.entry, "entry", mbr));
					bookmarks->InsertEndChild(b);
					}
				root->InsertEndChild(bookmarks);
				});
		if (debug_) std::cout << body << "\n";
		res.set_content(body, use_json ? "application/json" : "application/xml");
		});

	// deleteBookmark — remove a bookmark by song id.
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

		auto song_path = store_.song_path_by_id(std::stoi(it->second));
		if (!song_path || !store_.delete_bookmark(user, *song_path)) {
			auto msg = "Bookmark not found.";
			res.set_content(use_json ? subsonic_error_json(70, msg)
			                         : subsonic_error(70, msg),
			                use_json ? "application/json" : "application/xml");
			return;
			}

		std::string body = use_json ? subsonic_ok_json() : subsonic_ok();
		if (debug_) std::cout << body << "\n";
		res.set_content(body, use_json ? "application/json" : "application/xml");
		});

	// deletePlaylist — remove a playlist owned by the authenticated user.
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

		if (!store_.delete_playlist(std::stoi(it->second), user)) {
			auto msg = "Playlist not found.";
			res.set_content(use_json ? subsonic_error_json(70, msg)
			                         : subsonic_error(70, msg),
			                use_json ? "application/json" : "application/xml");
			return;
			}

		std::string body = use_json ? subsonic_ok_json() : subsonic_ok();
		if (debug_) std::cout << body << "\n";
		res.set_content(body, use_json ? "application/json" : "application/xml");
		});

	// listCastDevices — return the cached device list and kick off a background
	// refresh so the next call will have up-to-date results.
	server_.Get("/rest/listCastDevices.view", [this](const httplib::Request& req,
	                                                  httplib::Response& res) {
		if (!check_auth(req, res, store_)) return;
		bool use_json = (fmt_of(req) == "json");
		if (!check_cast_perm(req, res, store_, use_json)) return;

		auto devices = cast_manager_.cached_devices();
		cast_manager_.discover_background();

		std::string body;
		if (use_json) {
			body = subsonic_ok_json([&devices](nlohmann::json& r) {
				nlohmann::json arr = nlohmann::json::array();
				for (auto& d : devices)
					arr.push_back({{"id", d.id}, {"name", d.name},
					               {"address", d.address}, {"port", d.port}});
				r["castDevices"] = arr;
				});
			}
		else {
			body = subsonic_ok([&devices](XMLDocument& doc, XMLElement* root) {
				auto* el = doc.NewElement("castDevices");
				for (auto& d : devices) {
					auto* dev = doc.NewElement("castDevice");
					dev->SetAttribute("id",      d.id.c_str());
					dev->SetAttribute("name",    d.name.c_str());
					dev->SetAttribute("address", d.address.c_str());
					dev->SetAttribute("port",    d.port);
					el->InsertEndChild(dev);
					}
				root->InsertEndChild(el);
				});
			}

		res.set_content(body, use_json ? "application/json" : "application/xml");
		});

	// startCast — enter cast mode: subsequent stream requests go to the Chromecast.
	server_.Get("/rest/startCast.view", [this](const httplib::Request& req,
	                                            httplib::Response& res) {
		if (!check_auth(req, res, store_)) return;
		bool use_json = (fmt_of(req) == "json");
		if (!check_cast_perm(req, res, store_, use_json)) return;

		auto it = req.params.find("id");
		if (it == req.params.end()) {
			res.set_content(
				use_json ? subsonic_error_json(10, "Required parameter missing: id.")
				         : subsonic_error(10, "Required parameter missing: id."),
				use_json ? "application/json" : "application/xml");
			return;
			}

		auto devices = cast_manager_.cached_devices();
		CastManager::CastDevice chosen;
		bool found = false;
		for (auto& d : devices)
			if (d.id == it->second) { chosen = d; found = true; break; }

		if (!found) {
			res.set_content(
				use_json ? subsonic_error_json(70, "Cast device not found.")
				         : subsonic_error(70, "Cast device not found."),
				use_json ? "application/json" : "application/xml");
			return;
			}

		cast_manager_.start(chosen);
		res.set_content(use_json ? subsonic_ok_json() : subsonic_ok(),
		                use_json ? "application/json" : "application/xml");
		});

	// stopCast — stop Chromecast playback and exit cast mode.
	server_.Get("/rest/stopCast.view", [this](const httplib::Request& req,
	                                           httplib::Response& res) {
		if (!check_auth(req, res, store_)) return;
		bool use_json = (fmt_of(req) == "json");
		if (!check_cast_perm(req, res, store_, use_json)) return;
		cast_teardown();
		res.set_content(use_json ? subsonic_ok_json() : subsonic_ok(),
		                use_json ? "application/json" : "application/xml");
		});

	// castEvents — SSE stream that pushes MEDIA_STATUS updates to the browser.
	// Each event is a JSON object with playerState, currentTime, duration.
	// The connection is kept alive by the Chromecast heartbeat; a 15-second
	// keepalive comment is sent if no real update arrives in that window.
	//
	// This connection also acts as the cast session's heartbeat: when the
	// browser disconnects (tab closed, network drop, OS sleep) and no new
	// listener reconnects within CAST_IDLE_GRACE_S seconds, the watchdog
	// armed in the RAII guard's destructor tears the cast session down.
	server_.Get("/rest/castEvents.view", [this](const httplib::Request& req,
	                                            httplib::Response& res) {
		if (!check_auth(req, res, store_)) return;
		{
		bool use_json = (fmt_of(req) == "json");
		if (!check_cast_perm(req, res, store_, use_json)) return;
		}
		if (!cast_manager_.active()) { res.status = 204; return; }
		res.set_header("Cache-Control",    "no-cache");
		res.set_header("X-Accel-Buffering","no");   // disable nginx/apache buffering

		// RAII helper: increments the listener counter on construction and
		// arms the auto-stop watchdog on destruction.  Captured via
		// shared_ptr because httplib stores the content provider in a
		// std::function (which requires copyable callables); the guard's
		// destructor still fires exactly once, when the last copy of the
		// lambda is dropped — i.e. when the connection ends, regardless of
		// whether it ended via sink.write returning false (client gone),
		// wait_status seeing !active(), or normal completion.
		struct ListenerGuard {
			GainDrive* self;
			ListenerGuard(GainDrive* s) : self(s) {
				int n = ++self->cast_sse_listeners_;
				++self->cast_wd_gen_;
				if (self->debug_)
					std::cout << stamp() << "Cast: SSE listener attached, count="
					          << n << std::endl;
				}
			~ListenerGuard() {
				int n = --self->cast_sse_listeners_;
				int g = ++self->cast_wd_gen_;
				if (self->debug_)
					std::cout << stamp() << "Cast: SSE listener detached, count="
					          << n << std::endl;
				if (n != 0 || !self->cast_manager_.active()) return;
				GainDrive* gd = self;
				std::thread([gd, g] {
					std::this_thread::sleep_for(std::chrono::seconds(CAST_IDLE_GRACE_S));
					if (gd->cast_wd_gen_.load() != g)        return; // newer event
					if (gd->cast_sse_listeners_.load() != 0) return; // listener back
					if (!gd->cast_manager_.active())         return; // already stopped
					std::cout << stamp() << "Cast: no SSE listener for "
					          << CAST_IDLE_GRACE_S << "s, auto-stopping"
					          << std::endl;
					gd->cast_teardown();
					}).detach();
				}
			};
		auto guard = std::make_shared<ListenerGuard>(this);

		res.set_chunked_content_provider("text/event-stream",
			[this, guard](size_t, httplib::DataSink& sink) -> bool {
				auto s = cast_manager_.wait_status(15000);
				if (!cast_manager_.active()) return false;
				std::string event = "data: " + nlohmann::json({
					{"playerState", s.player_state},
					{"currentTime", s.current_time},
					{"duration",    s.duration},
					{"idleReason",  s.idle_reason},
					{"startOffset", last_cast_offset_}}).dump() + "\n\n";
				return sink.write(event.data(), event.size());
				});
		});

	// castSession — non-blocking snapshot of current cast session state.
	// Used by the browser on page load to restore the cast UI after a reload.
	server_.Get("/rest/castSession.view", [this](const httplib::Request& req,
	                                             httplib::Response& res) {
		if (!check_auth(req, res, store_)) return;
		bool use_json = (fmt_of(req) == "json");
		if (!check_cast_perm(req, res, store_, use_json)) return;

		if (!cast_manager_.active()) {
			res.set_content(subsonic_ok_json([](nlohmann::json& r) {
				r["castSession"]["active"] = false;
				}), "application/json");
			return;
			}

		auto st = cast_manager_.get_status();
		double song_duration = 0.0;
		if (!last_cast_song_id_.empty()) {
			auto song = store_.get_song(std::stoi(last_cast_song_id_));
			if (song) song_duration = song->duration;
			}
		res.set_content(subsonic_ok_json([&](nlohmann::json& r) {
			r["castSession"]["active"]       = true;
			r["castSession"]["deviceId"]     = cast_manager_.get_device_id();
			r["castSession"]["deviceName"]   = cast_manager_.get_device_name();
			r["castSession"]["songId"]       = last_cast_song_id_;
			r["castSession"]["startOffset"]  = last_cast_offset_;
			r["castSession"]["playerState"]  = st.player_state;
			r["castSession"]["currentTime"]  = st.current_time;
			r["castSession"]["duration"]     = st.duration;
			r["castSession"]["songDuration"] = song_duration;
			}), "application/json");
		});

	// castControl — send play/pause/seek to the Chromecast.
	server_.Get("/rest/castControl.view", [this](const httplib::Request& req,
	                                             httplib::Response& res) {
		if (!check_auth(req, res, store_)) return;
		bool use_json = (fmt_of(req) == "json");
		if (!check_cast_perm(req, res, store_, use_json)) return;
		std::string action;
		auto ai = req.params.find("action");
		if (ai != req.params.end()) action = ai->second;

		if (action == "pause")      cast_manager_.cast_pause();
		else if (action == "play") {
			auto st = cast_manager_.get_status();
			if (st.player_state == "PAUSED") {
				cast_manager_.cast_play();
				} else if (st.player_state == "IDLE" && !last_cast_song_id_.empty()) {
				// Session timed out during a long pause — re-issue a full load from
				// the saved position so the Chromecast can restart the stream.
				auto song = store_.get_song(std::stoi(last_cast_song_id_));
				if (song) {
					std::string host  = req.get_header_value("Host");
					if (host.empty()) host = "localhost";
					std::string proto = req.get_header_value("X-Forwarded-Proto");
					if (proto.empty()) proto = "http";
					// last_known_time is relative for non-MP3 (server-side seek)
					// and absolute for MP3 (native seek); adding last_cast_offset_
					// always yields the absolute song position.
					float pos = last_cast_offset_ + cast_manager_.last_known_time();
					std::string url = proto + "://" + host + "/rest/stream.view"
					                + "?id=" + last_cast_song_id_
					                + "&castToken=" + cast_manager_.token();
					// Same MP3-vs-other split as castLoad.view.
					if (pos > 0.5f && song->codec != "mp3") {
						url += "&timeOffset="
						     + std::to_string(static_cast<int>(pos));
						last_cast_offset_ = pos;
						cast_manager_.load(url, codec_to_mime(song->codec),
						                   0.0f, song->duration);
						}
					else {
						last_cast_offset_ = 0.0f;
						cast_manager_.load(url, codec_to_mime(song->codec),
						                   pos > 0.5f ? pos : 0.0f,
						                   song->duration);
						}
					}
				}
			}
		else if (action == "seek") {
			auto ti = req.params.find("time");
			if (ti != req.params.end())
				cast_manager_.cast_seek(std::stof(ti->second));
			}

		res.set_content(use_json ? subsonic_ok_json() : subsonic_ok(),
		                use_json ? "application/json" : "application/xml");
		});

	// castLoad — instruct the Chromecast to fetch and play a song.
	// Separate from stream.view so the browser triggers the cast load without
	// making a Range request that httplib would reject (stream.view returns 204,
	// but httplib overrides 204+Range to 416 when content_length is 0).
	server_.Get("/rest/castLoad.view", [this](const httplib::Request& req,
	                                          httplib::Response& res) {
		if (!check_auth(req, res, store_)) return;
		bool use_json = (fmt_of(req) == "json");
		if (!check_cast_perm(req, res, store_, use_json)) return;

		auto err = [&](int code, const char* msg) {
			res.set_content(use_json ? subsonic_error_json(code, msg)
			                         : subsonic_error(code, msg),
			                use_json ? "application/json" : "application/xml");
			};

		if (!cast_manager_.active()) { err(0, "Cast not active."); return; }

		auto it = req.params.find("id");
		if (it == req.params.end()) {
			err(10, "Required parameter missing: id."); return;
			}

		auto song = store_.get_song(std::stoi(it->second));
		if (!song) { err(70, "Song not found."); return; }

		std::string host  = req.get_header_value("Host");
		if (host.empty()) host = "localhost";
		std::string proto = req.get_header_value("X-Forwarded-Proto");
		if (proto.empty()) proto = "http";
		std::string url = proto + "://" + host + "/rest/stream.view"
		                + "?id=" + it->second
		                + "&castToken=" + cast_manager_.token();
		auto to_it = req.params.find("timeOffset");
		float cast_offset = 0.0f;
		if (to_it != req.params.end() && !to_it->second.empty())
			cast_offset = std::stof(to_it->second);
		last_cast_song_id_ = it->second;
		// Native seek (currentTime in LOAD, full file at the URL) only works
		// for MP3.  For FLAC the Default Media Receiver echoes the requested
		// currentTime in MEDIA_STATUS but actually plays from byte 0; other
		// non-MP3 formats are presumed to have the same problem.  Fall back
		// to server-side seek for those: encode the seek into the URL and
		// tell the receiver currentTime=0 so it doesn't try to seek further.
		if (cast_offset > 0.0f && song->codec != "mp3") {
			url += "&timeOffset=" + std::to_string(static_cast<int>(cast_offset));
			last_cast_offset_ = cast_offset;
			cast_manager_.load(url, codec_to_mime(song->codec),
			                   0.0f, song->duration);
			}
		else {
			last_cast_offset_ = 0.0f;
			cast_manager_.load(url, codec_to_mime(song->codec),
			                   cast_offset, song->duration);
			}
		res.set_content(use_json ? subsonic_ok_json() : subsonic_ok(),
		                use_json ? "application/json" : "application/xml");
		});

	// updateSong — update title and/or track number for a single song.
	// Writes the change to the database and back to the audio file tags.
	server_.Get("/rest/updateSong.view", [this](const httplib::Request& req,
	                                            httplib::Response& res) {
		if (!check_auth(req, res, store_)) return;
		bool use_json = (fmt_of(req) == "json");

		auto err = [&](int code, const char* msg) {
			res.set_content(use_json ? subsonic_error_json(code, msg)
			                         : subsonic_error(code, msg),
			                use_json ? "application/json" : "application/xml");
			};

		auto it = req.params.find("id");
		if (it == req.params.end()) { err(10, "Required parameter missing: id."); return; }
		int song_id = std::stoi(it->second);

		std::optional<std::string> title;
		std::optional<int> track_number;
		std::optional<int> year;
		std::optional<int> disc_number;
		if (req.params.count("title")) title        = req.params.find("title")->second;
		if (req.params.count("track")) track_number = std::stoi(req.params.find("track")->second);
		if (req.params.count("year"))  year         = std::stoi(req.params.find("year")->second);
		if (req.params.count("disc"))  disc_number  = std::stoi(req.params.find("disc")->second);

		// Resolve the file path before touching anything.
		auto song = store_.get_song(song_id);
		if (!song) { err(70, "Song not found."); return; }

		// Write tags first — if this fails we must not update the database.
		try {
			std::string song_abs = store_.abs_path(song->path);
			if (!store_.path_is_within_root(song_abs)) {
				std::cout << stamp() << "updateSong: refusing path outside music_root: "
				          << song_abs << std::endl;
				err(0, "Refusing to write outside music_root.");
				return;
				}
			TagLib::FileRef f(song_abs.c_str());
			if (f.isNull() || !f.tag()) {
				err(0, "Could not open file for tag editing.");
				return;
				}
			if (title)        f.tag()->setTitle(TagLib::String(*title, TagLib::String::UTF8));
			if (track_number) f.tag()->setTrack(*track_number);
			if (year)         f.tag()->setYear(*year);
			if (disc_number) {
				// PropertyMap gives portable access to DISCNUMBER across all formats.
				TagLib::PropertyMap props = f.file()->properties();
				props.replace("DISCNUMBER",
					TagLib::StringList(TagLib::String(std::to_string(*disc_number))));
				f.file()->setProperties(props);
				}
			if (!f.save()) {
				err(0, "Could not write tags to file.");
				return;
				}
			}
		catch (...) {
			err(0, "Exception while writing tags to file.");
			return;
			}

		// Tags written successfully — now mirror the change in the database.
		store_.update_song_meta(song_id, title, track_number, year, disc_number);

		res.set_content(use_json ? subsonic_ok_json() : subsonic_ok(),
		                use_json ? "application/json" : "application/xml");
		});

	// setCoverArt — set a cover image for an album (identified by folder_id).
	// Accepts either a multipart "file" part or a "url" form field; for the
	// latter the server fetches the URL and validates it returned an image.
	server_.Post("/rest/setCoverArt.view", [this](const httplib::Request& req,
	                                              httplib::Response& res) {
		if (!check_auth(req, res, store_)) return;
		bool use_json = (fmt_of(req) == "json");

		auto err = [&](int code, const char* msg) {
			res.set_content(use_json ? subsonic_error_json(code, msg)
			                         : subsonic_error(code, msg),
			                use_json ? "application/json" : "application/xml");
			};

		auto it = req.params.find("id");
		if (it == req.params.end()) { err(10, "Required parameter missing: id."); return; }
		int folder_id = std::stoi(it->second);

		std::string bytes;
		std::string url;
		if (req.has_file("url")) url = req.get_file_value("url").content;
		else if (req.has_param("url")) url = req.get_param_value("url");

		if (req.has_file("file")) {
			const auto& fp = req.get_file_value("file");
			if (fp.content_type.rfind("image/", 0) != 0) {
				err(0, "Uploaded file must be an image."); return;
				}
			bytes = fp.content;
			}
		else if (!url.empty()) {
			bool https = url.rfind("https://", 0) == 0;
			bool http  = url.rfind("http://",  0) == 0;
			if (!https && !http) { err(0, "URL must be http(s)."); return; }
			size_t scheme_end = https ? 8 : 7;
			size_t slash = url.find('/', scheme_end);
			std::string host = url.substr(scheme_end, slash == std::string::npos
			                              ? std::string::npos : slash - scheme_end);
			std::string path = (slash == std::string::npos) ? "/" : url.substr(slash);

			auto fetch = [&](auto& cli) {
				cli.set_follow_location(true);
				cli.set_default_headers({
					{"User-Agent",
					 "GainDrive/0.1 (https://github.com/kpeeters/gaindrive)"}
					});
				return cli.Get(path.c_str());
				};
			httplib::Result r;
			if (https) { httplib::SSLClient cli(host); r = fetch(cli); }
			else       { httplib::Client    cli(host); r = fetch(cli); }
			if (!r || r->status != 200) { err(0, "Failed to fetch URL."); return; }
			auto ct = r->get_header_value("Content-Type");
			if (ct.rfind("image/", 0) != 0) {
				err(0, "URL did not return an image."); return;
				}
			bytes = std::move(r->body);
			}
		else {
			err(10, "Required parameter missing: file or url."); return;
			}

		std::string folder_rel = store_.get_folder_path(folder_id);
		if (folder_rel.empty()) { err(70, "Album folder not found."); return; }

		namespace fs = std::filesystem;
		fs::path cover_rel = fs::path(folder_rel) / "cover.jpg";
		fs::path cover_abs = fs::path(store_.abs_path(cover_rel.string()));
		if (!store_.path_is_within_root(cover_abs)) {
			std::cout << stamp() << "setCoverArt: refusing path outside music_root: "
			          << cover_abs.string() << std::endl;
			err(0, "Refusing to write outside music_root.");
			return;
			}
		{
		std::ofstream out(cover_abs, std::ios::binary | std::ios::trunc);
		if (!out) { err(0, "Failed to write cover art to disk."); return; }
		out.write(bytes.data(), (std::streamsize)bytes.size());
		}

		store_.set_cover_art_path(folder_id, cover_rel.string());

		res.set_content(use_json ? subsonic_ok_json() : subsonic_ok(),
		                use_json ? "application/json" : "application/xml");
		});

	// Upload a music archive (zip / tar / tar.gz / tgz) to the staging directory.
	server_.Post("/upload", [this](const httplib::Request& req, httplib::Response& res) {
		if (!check_auth(req, res, store_)) return;

		auto json_err = [&](const std::string& msg) {
			nlohmann::json j;
			j["status"]  = "error";
			j["message"] = msg;
			res.status = 400;
			res.set_content(j.dump(), "application/json");
			};

		// Require upload_allowed (or admin).
		{
		auto it = req.params.find("u");
		auto ui = (it != req.params.end()) ? store_.get_user(it->second) : std::nullopt;
		if (!ui || (!ui->upload_allowed && !ui->is_admin)) {
			json_err("User is not authorized to upload.");
			return;
			}
		}

		if (!req.has_file("file")) { json_err("Missing file part."); return; }
		const auto& fp = req.get_file_value("file");

		// Validate extension.
		std::string name = fp.filename;
		bool ok = name.ends_with(".zip")
		       || name.ends_with(".tar")
		       || name.ends_with(".tar.gz")
		       || name.ends_with(".tgz");
		if (!ok) { json_err("Unsupported file type. Use zip, tar, tar.gz, or tgz."); return; }

		// Build a timestamped, filesystem-safe destination name.
		std::string safe;
		for (char c : name)
			safe += (std::isalnum(c) || c == '.' || c == '-' || c == '_') ? c : '_';

		auto now = std::chrono::system_clock::now();
		auto tt  = std::chrono::system_clock::to_time_t(now);
		char ts[32];
		std::strftime(ts, sizeof(ts), "%Y%m%dT%H%M%S", std::gmtime(&tt));
		std::string dest_name = std::string(ts) + "_" + safe;

		namespace fs = std::filesystem;
		fs::path dest = fs::path(upload_dir_) / dest_name;
		{
		std::ofstream out(dest, std::ios::binary | std::ios::trunc);
		if (!out) { json_err("Failed to write file to upload directory."); return; }
		out.write(fp.content.data(), (std::streamsize)fp.content.size());
		}

		std::cout << stamp() << "Upload received: " << dest_name
		          << " (" << fp.content.size() << " bytes)" << std::endl;

		nlohmann::json j;
		j["status"]   = "ok";
		j["filename"] = dest_name;
		res.set_content(j.dump(), "application/json");
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
	cast_manager_.discover_background();
	watcher_.start();
	}

void GainDrive::cast_teardown()
	{
	cast_manager_.stop();
	last_cast_song_id_.clear();
	last_cast_offset_ = 0.0f;
	}

void GainDrive::listen(const std::string& host, int port)
	{
	std::cout << stamp() << "Listening on " << host << ":" << port << std::endl;
	server_.listen(host, port);
	}
