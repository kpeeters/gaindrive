#include "gaindrive.hh"
#include "stamp.hh"
#include "streamer.hh"
#include "codecs.hh"
#include "embedded_web.hh"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <map>
#include <memory>
#include <random>
#include <set>
#include <sstream>
#include <thread>

#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>

#include <reproc++/reproc.hpp>
#include <reproc++/drain.hpp>

#include <archive.h>
#include <archive_entry.h>

#include <tinyxml2.h>
#include <nlohmann/json.hpp>
#include <taglib/fileref.h>
#include <taglib/tag.h>
#include <tpropertymap.h>

using namespace tinyxml2;

// ---- Subsonic XML helpers ---------------------------------------------

static const char* SUBSONIC_NS  = "http://subsonic.org/restapi";
static const char* SUBSONIC_VER = "1.16.1";

// OpenSubsonic requires a server that sets openSubsonic=true to also identify
// itself: `type` is the implementation, `serverVersion` is our own version as
// distinct from the API version above.
static const char* SERVER_TYPE    = "gaindrive";
static const char* SERVER_VERSION = "0.1";

// Subsonic ids are strings in the API even though they are row ids here. Every
// id crossing the wire in JSON goes through this — the XML path renders
// attributes as text anyway, so it needs no equivalent.
static std::string sid(int id)
	{
	return std::to_string(id);
	}

// Numeric query params, without letting a malformed one escape the handler.
// std::stoi throws on garbage and on overflow; httplib turns that into a bare
// HTTP 500, which no Subsonic client can interpret — they expect a 200 with an
// <error> body.  See ISSUES.md for the sites still unguarded.
static int to_int(const std::string& s, int def)
	{
	if (s.empty()) return def;
	try { return std::stoi(s); } catch (...) { return def; }
	}

static float to_float(const std::string& s, float def)
	{
	if (s.empty()) return def;
	try { return std::stof(s); } catch (...) { return def; }
	}

// SQLite CURRENT_TIMESTAMP formats as "YYYY-MM-DD HH:MM:SS" in UTC, but the
// API wants ISO 8601. Same instant, different spelling. Empty in, empty out,
// so callers can keep using emptiness to mean "absent".
static std::string iso8601(const std::string& ts)
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

static std::string subsonic_error_json(int code, const char* msg)
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

// What stream.view would actually send for this song, when that differs from
// the stored file.  Mirrors the branch order in Streamer::serve() — a format
// override wins over the per-user bitrate cap, and the cap alone means mp3.
// Empty when the source is served as-is.
struct TranscodeInfo
	{
	std::string_view mime;
	std::string_view suffix;
	int              bitrate;
	};

static std::optional<TranscodeInfo> transcode_target(
	const MediaStore::ChildEntry& c, int max_bitrate,
	const std::string& format = "")
	{
	// Video: the container is what changes, not the audio muxer, so none of
	// the negotiation below applies.  Anything that is not already in a
	// browser-playable container arrives as MP4, whether that took a remux or
	// a full re-encode.  Whether it is *seekable* is a different question and
	// is answered separately by nativeSeek — see song_entry_json().
	if (is_video_ext(c.codec)) {
		if (browser_container(c.codec)
		        && video_seeks_natively(c.video_codec, c.audio_codec))
			return std::nullopt;   // served untouched
		return TranscodeInfo{ VIDEO_MP4_MIME, "mp4", 0 };
		}

	auto source = target_for(c.codec);
	std::optional<Target> wanted;
	if (!format.empty() && format != "raw")
		wanted = target_for(format);
	// Same muxer and encoder is the same audio, whatever the caller spelled it.
	if (wanted && (!source || source->muxer   != wanted->muxer
	                       || source->encoder != wanted->encoder)) {
		// Keep this rule identical to the one in Streamer::serve(); a client
		// that trusts transcodedBitRate and then receives something else has
		// no way to tell which of the two lied.
		int bitrate = (max_bitrate > 0 && max_bitrate < 320) ? max_bitrate : 320;
		return TranscodeInfo{ wanted->mime, wanted->name, bitrate };
		}
	if (max_bitrate <= 0 || c.bitrate <= 0 || c.bitrate <= max_bitrate)
		return std::nullopt;
	return TranscodeInfo{ "audio/mpeg", "mp3", max_bitrate };
	}

// Serialises a song ChildEntry into a JSON object.  When max_bitrate causes a
// transcode, also emits transcodedContentType / transcodedSuffix (standard
// Subsonic) and transcodedBitRate (gaindrive extension; ignored by clients
// that don't know it) so the client knows the actual stream format.
static nlohmann::json song_entry_json(const MediaStore::ChildEntry& c,
                                       int max_bitrate = 0,
                                       const std::string& format = "")
	{
	// Video rows live in the same table and come back through the same
	// queries; the extension is what distinguishes them, so codecs.hh answers
	// this without a dedicated column travelling through every query.
	bool is_video = is_video_ext(c.codec);
	nlohmann::json s = {
		{"id",          sid(c.id)},
		{"parent",      sid(c.parent_id)},
		// The album folder is the song's album in ID3 terms; clients asking
		// for tag-based data expect albumId rather than parent.
		{"albumId",     sid(c.parent_id)},
		{"isDir",       false},
		{"type",        is_video ? "video" : "music"},
		{"isVideo",     is_video},
		{"title",       c.title},
		{"artist",      c.artist},
		{"album",       c.album},
		{"track",       c.track_number},
		{"discNumber",  c.disc_number},
		{"year",        c.year},
		{"genre",       c.genre},
		{"size",        c.file_size},
		{"contentType", std::string(codec_to_mime(c.codec))},
		{"suffix",      c.codec},
		{"duration",    (int)c.duration},
		{"bitRate",     c.bitrate}
		};
	if (c.cover_art_id >= 0) s["coverArt"] = sid(c.cover_art_id);
	if (!c.starred.empty()) s["starred"] = iso8601(c.starred);
	// Omitted rather than sent as 0 when the scan could not probe the file, or
	// when the query that produced this entry does not select the dimensions.
	if (c.width  > 0) s["originalWidth"]  = c.width;
	if (c.height > 0) s["originalHeight"] = c.height;
	if (auto t = transcode_target(c, max_bitrate, format)) {
		s["transcodedContentType"] = std::string(t->mime);
		s["transcodedSuffix"]      = std::string(t->suffix);
		// Video has no meaningful single bitrate to promise — the encode is
		// CRF-driven — so the field is omitted rather than sent as 0.
		if (t->bitrate > 0) s["transcodedBitRate"] = t->bitrate;
		}
	// gaindrive extension.  Says whether the stream this entry would produce
	// carries a Content-Length and answers Range requests, so the client can
	// let the media element seek by itself instead of re-requesting with
	// timeOffset.  transcodedSuffix cannot answer this: it is present for both
	// the remux and re-encode tiers, and those differ precisely here.
	if (is_video) s["nativeSeek"] = video_seeks_natively(c.video_codec,
	                                                     c.audio_codec);
	return s;
	}

// Creates an XML element for a song with the given tag name.
static XMLElement* song_entry_xml(XMLDocument& doc,
                                   const MediaStore::ChildEntry& c,
                                   const char* tag,
                                   int max_bitrate = 0,
                                   const std::string& format = "")
	{
	bool  is_video = is_video_ext(c.codec);
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
	el->SetAttribute("contentType", std::string(codec_to_mime(c.codec)).c_str());
	el->SetAttribute("suffix",      c.codec.c_str());
	el->SetAttribute("duration",    (int)c.duration);
	el->SetAttribute("bitRate",     c.bitrate);
	el->SetAttribute("type",        is_video ? "video" : "music");
	el->SetAttribute("isVideo",     is_video);
	el->SetAttribute("albumId",     c.parent_id);
	if (c.width  > 0) el->SetAttribute("originalWidth",  c.width);
	if (c.height > 0) el->SetAttribute("originalHeight", c.height);
	if (!c.starred.empty())
		el->SetAttribute("starred", iso8601(c.starred).c_str());
	if (auto t = transcode_target(c, max_bitrate, format)) {
		el->SetAttribute("transcodedContentType", std::string(t->mime).c_str());
		el->SetAttribute("transcodedSuffix",      std::string(t->suffix).c_str());
		if (t->bitrate > 0)
			el->SetAttribute("transcodedBitRate", t->bitrate);
		}
	if (is_video)
		el->SetAttribute("nativeSeek",
		                 video_seeks_natively(c.video_codec, c.audio_codec));
	return el;
	}

// Bridges MediaStore's song record to the streamer's.  Written once because
// the video fields are easy to forget in an aggregate initialiser — and a
// dropped is_video sends a video down the audio ladder, where TARGETS has no
// entry for its container and the whole tier decision is skipped.
// size_override exists for the Cast probe, which deliberately serves a slice.
static Streamer::SongInfo streamer_song(const MediaStore::SongInfo& s,
                                         const std::string& abs,
                                         int64_t size_override = 0)
	{
	Streamer::SongInfo si{ abs, s.codec, s.bitrate, s.duration,
	                       size_override > 0 ? size_override : s.file_size,
	                       s.id, s.file_modified };
	si.is_video    = s.is_video;
	si.width       = s.width;
	si.height      = s.height;
	si.video_codec = s.video_codec;
	si.audio_codec = s.audio_codec;
	return si;
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
				{"id",        sid(pl.id)},
				{"name",      pl.name},
				{"comment",   pl.comment},
				{"owner",     pl.owner},
				{"public",    pl.is_public},
				{"songCount", pl.song_count},
				{"duration",  pl.duration},
				{"created",   iso8601(pl.created)},
				{"changed",   iso8601(pl.updated)},
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
		playlist->SetAttribute("created",   iso8601(pl.created).c_str());
		playlist->SetAttribute("changed",   iso8601(pl.updated).c_str());
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
//
// The result is buffered and sent with set_content() rather than streamed.
// That is not an optimisation, it is the framing: the no-length content
// provider this used to call emits neither Content-Length nor
// Transfer-Encoding, and httplib only sends Connection: close when the
// connection is closing for unrelated reasons.  The response therefore went
// out on a keep-alive connection with nothing marking where the body ended,
// the client read on into the following response, and every image after the
// first on that connection was the previous one's — which is what "all the
// thumbnails are wrong, and reloading doesn't help" looks like.  CLAUDE.md
// records the same hazard for serve_transcoded; this call site never got it.
//
// A scaled cover is a few hundred KB at most, so there is nothing to gain from
// streaming it, and buffering also lets the caller hash the bytes for an ETag.
static void serve_cover_scaled(httplib::Response& res,
                                const std::string& path, int size)
	{
	std::vector<std::string> args = {
		"ffmpeg", "-v", "quiet", "-i", path,
		"-vf", "scale=" + std::to_string(size) + ":" + std::to_string(size)
		       + ":force_original_aspect_ratio=decrease",
		"-frames:v", "1", "-f", "mjpeg", "pipe:1"
		};

	reproc::process proc;
	reproc::options opts;
	opts.redirect.err.type = reproc::redirect::type::discard;

	if (proc.start(args, opts)) {
		std::cout << stamp() << "getCoverArt: ffmpeg launch failed for "
		          << path << std::endl;
		res.status = 500;
		return;
		}

	std::string          out;
	reproc::sink::string sink(out);
	// drain() checks the error code itself, which is the point: reading by
	// hand needs `n == 0 || err`, because reproc wraps a negative return into
	// size_t and the old `n == 0` test let a huge length reach sink.write().
	auto ec            = reproc::drain(proc, sink, reproc::sink::null);
	auto [status, wec] = proc.wait(reproc::infinite);

	if (ec || wec || status != 0 || out.empty()) {
		// A zero-length 200 renders as a broken image and looks like a
		// missing file; say what actually happened instead.
		std::cout << stamp() << "getCoverArt: ffmpeg failed for " << path
		          << " (status=" << status << ", " << out.size() << " bytes)"
		          << std::endl;
		res.status = 500;
		return;
		}

	res.set_content(out, "image/jpeg");
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

// Performs MusicBrainz/Wikipedia lookup for an artist, caching the result.
// Returns cached data immediately when available; triggers a fresh fetch otherwise.
static MediaStore::CachedArtistInfo resolve_artist_info(int id, const std::string& name,
                                                         MediaStore& store, bool force = false)
	{
	// A level-1 folder of a categories root is a section — Film, Series,
	// Documentary — not a performer.  Looking it up would query MusicBrainz
	// for "Film" and cache whatever came back as that section's biography.
	// Same failure shape as the CD1 lookup recorded in ISSUES.md.
	if (store.is_category_folder(id)) {
		std::cout << stamp() << "getArtistInfo [" << name
		          << "] is a category, not an artist; skipping lookup"
		          << std::endl;
		return {};
		}

	if (!force) {
		auto cached = store.get_cached_artist_info(id);
		if (cached) {
			std::cout << stamp() << "getArtistInfo [" << name << "] cached"
			          << " mbid=" << (cached->mbid.empty() ? "(none)" : cached->mbid)
			          << std::endl;
			return *cached;
			}
		}

	std::cout << stamp() << "getArtistInfo [" << name << "] querying MusicBrainz"
	          << std::endl;
	MediaStore::CachedArtistInfo info;
	httplib::SSLClient mb("musicbrainz.org");
	mb.set_default_headers({
		{"User-Agent", "GainDrive/0.1 (https://github.com/kpeeters/gaindrive)"}
		});
	httplib::Params params{
		{"query", "artist:\"" + name + "\""},
		{"limit", "1"},
		{"fmt",   "json"}
		};
	bool mb_ok = false;
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
		mb_ok = true;
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
			std::string wd_image_url;
			for (auto& rel : rels) {
				std::string type     = rel.value("type","");
				std::string resource = rel.value("url", nlohmann::json::object())
				                          .value("resource","");
				if (type == "allmusic" && info.allmusic_url.empty()) {
					info.allmusic_url = resource;
					std::cout << stamp() << "getArtistInfo [" << name
					          << "] AllMusic: " << resource << std::endl;
					}
				else if (type == "discogs" && info.discogs_url.empty()) {
					info.discogs_url = resource;
					std::cout << stamp() << "getArtistInfo [" << name
					          << "] Discogs: " << resource << std::endl;
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
							{"props","sitelinks|claims"},{"sitefilter","enwiki"},
							{"format","json"}
							},
						httplib::Headers{});
					if (rwd && rwd->status == 200) {
						auto jwd = nlohmann::json::parse(rwd->body, nullptr, false);
						if (!jwd.is_discarded()) {
							auto& ents = jwd["entities"];
							if (ents.contains(entity)
							        && ents[entity].contains("sitelinks")
							        && ents[entity]["sitelinks"].contains("enwiki")) {
								wiki_title = ents[entity]["sitelinks"]["enwiki"]
								                .value("title","");
								if (!wiki_title.empty())
									std::cout << stamp() << "getArtistInfo [" << name
									          << "] Wikipedia (via Wikidata): "
									          << wiki_title << std::endl;
								}
							// Wikidata P18 (image) as fallback when no Wikipedia article.
							if (ents.contains(entity) && ents[entity].contains("claims")) {
								auto& claims = ents[entity]["claims"];
								if (claims.contains("P18") && !claims["P18"].empty()) {
									std::string fn = claims["P18"][0]["mainsnak"]["datavalue"]
									                    .value("value","");
									if (!fn.empty()) {
										for (char& c : fn) if (c == ' ') c = '_';
										wd_image_url =
											"https://commons.wikimedia.org/wiki/Special:FilePath/"
											+ url_encode(fn);
										}
									}
								}
							}
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
			if (info.image_url.empty() && !wd_image_url.empty()) {
				info.image_url = wd_image_url;
				std::cout << stamp() << "getArtistInfo [" << name
				          << "] image from Wikidata P18: " << wd_image_url << std::endl;
				}

			if (info.image_url.empty()) {
				std::this_thread::sleep_for(std::chrono::seconds(1));
				httplib::SSLClient tadb("www.theaudiodb.com");
				tadb.set_default_headers({
					{"User-Agent","GainDrive/0.1 (https://github.com/kpeeters/gaindrive)"}
					});
				auto rt = tadb.Get("/api/v1/json/2/artist-mb.php",
				                   httplib::Params{{"i", info.mbid}},
				                   httplib::Headers{});
				if (rt && rt->status == 200) {
					auto jt = nlohmann::json::parse(rt->body, nullptr, false);
					if (!jt.is_discarded() && !jt["artists"].is_null()
					        && !jt["artists"].empty()) {
						info.image_url = jt["artists"][0].value("strArtistThumb","");
						if (!info.image_url.empty())
							std::cout << stamp() << "getArtistInfo [" << name
							          << "] image from TheAudioDB: "
							          << info.image_url << std::endl;
						}
					}
				}

			if (info.image_url.empty() && !info.discogs_url.empty()) {
				std::string token = store.get_setting("discogs_token");
				if (token.empty()) {
					std::cout << stamp() << "getArtistInfo [" << name
					          << "] Discogs URL available but no token configured"
					          << std::endl;
					}
				else {
					auto apos = info.discogs_url.find("/artist/");
					if (apos != std::string::npos) {
						std::string id_str;
						for (char c : info.discogs_url.substr(apos + 8))
							{ if (!std::isdigit(c)) break; id_str += c; }
						if (!id_str.empty()) {
							std::this_thread::sleep_for(std::chrono::seconds(1));
							httplib::SSLClient disc("api.discogs.com");
							disc.set_default_headers({
								{"User-Agent",
								 "GainDrive/0.1 (https://github.com/kpeeters/gaindrive)"},
								{"Authorization", "Discogs token=" + token}
								});
							auto rd = disc.Get("/artists/" + id_str,
							                   httplib::Params{}, httplib::Headers{});
							if (rd && rd->status == 200) {
								auto jd = nlohmann::json::parse(rd->body, nullptr, false);
								if (!jd.is_discarded() && jd.contains("images")
								        && !jd["images"].empty()) {
									std::string uri;
									for (auto& img : jd["images"])
										if (img.value("type","") == "primary")
											{ uri = img.value("uri",""); break; }
									if (uri.empty())
										uri = jd["images"][0].value("uri","");
									if (!uri.empty()) {
										info.image_url = uri;
										std::cout << stamp() << "getArtistInfo [" << name
										          << "] image from Discogs API: "
										          << uri << std::endl;
										}
									}
								}
							else {
								std::cout << stamp() << "getArtistInfo [" << name
								          << "] Discogs API failed"
								          << (rd ? " HTTP " + std::to_string(rd->status)
								                : " (no response)")
								          << std::endl;
								}
							}
						}
					}
				}

			if (info.image_url.empty())
				std::cout << stamp() << "getArtistInfo [" << name
				          << "] no image found" << std::endl;
			}
		}

	if (mb_ok) {
		store.cache_artist_info(id, info);
		std::cout << stamp() << "getArtistInfo [" << name << "] cached"
		          << " mbid=" << (info.mbid.empty() ? "(none)" : info.mbid)
		          << std::endl;
		}
	else {
		std::cout << stamp() << "getArtistInfo [" << name
		          << "] not cached (MusicBrainz unavailable)" << std::endl;
		}
	return info;
	}

// Shared implementation for getArtistInfo and getArtistInfo2.
// key is "artistInfo" or "artistInfo2" — controls the XML element / JSON key.
static void handle_artist_info(const httplib::Request& req, httplib::Response& res,
                                MediaStore& store, const char* key)
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

	bool force = req.params.count("force") > 0
	          && req.params.find("force")->second != "0";
	auto info = resolve_artist_info(id, name, store, force);

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
			if (!info.discogs_url.empty())    ai["discogsUrl"]    = info.discogs_url;
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
			add_text_el(doc, ai, "discogsUrl",    info.discogs_url);
			add_text_el(doc, ai, "smallImageUrl",  info.image_url);
			add_text_el(doc, ai, "mediumImageUrl", info.image_url);
			add_text_el(doc, ai, "largeImageUrl",  info.image_url);
			root->InsertEndChild(ai);
			});
	res.set_content(body, use_json ? "application/json" : "application/xml");
	}

// ---- Album info helper -----------------------------------------------

// Shared implementation for getAlbumInfo and getAlbumInfo2.
// Searches MusicBrainz for the release-group, then resolves a Wikipedia
// article via Wikidata if needed. Results are cached in album_info_cache.
static void handle_album_info(const httplib::Request& req, httplib::Response& res,
                               MediaStore& store)
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
	res.set_content(body, use_json ? "application/json" : "application/xml");
	}

// ---- Album list helper -----------------------------------------------

// Shared implementation for getAlbumList and getAlbumList2.
// key is "albumList" or "albumList2".
static void handle_album_list(const httplib::Request& req, httplib::Response& res,
                               MediaStore& store, const char* key)
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
					{"duration",  al.duration}
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

// ---- GainDrive --------------------------------------------------------

static std::string make_uuid()
   {
   std::random_device rd;
   std::mt19937_64 gen(rd());
   std::uniform_int_distribution<uint64_t> dis;
   uint64_t a = dis(gen), b = dis(gen);
   // Set UUID v4 version and variant bits.
   a = (a & 0xFFFFFFFFFFFF0FFFull) | 0x0000000000004000ull;
   b = (b & 0x3FFFFFFFFFFFFFFFull) | 0x8000000000000000ull;
   std::ostringstream ss;
   ss << std::hex << std::setfill('0')
      << std::setw(8)  << (uint32_t)(a >> 32)        << '-'
      << std::setw(4)  << (uint32_t)((a >> 16) & 0xFFFF) << '-'
      << std::setw(4)  << (uint32_t)(a & 0xFFFF)     << '-'
      << std::setw(4)  << (uint32_t)(b >> 48)         << '-'
      << std::setw(12) << (b & 0x0000FFFFFFFFFFFFull);
   return ss.str();
   }

// Extract a zip/tar/tar.gz/tgz archive from memory into dest_dir.
// Entry paths are sanitised: absolute components and ".." are stripped so
// no file can escape dest_dir. Returns the number of regular files written,
// or -1 if the archive could not be opened.
static int extract_archive_to_dir(const std::string& content,
                                   const std::filesystem::path& dest_dir)
   {
   namespace fs = std::filesystem;

   struct archive* a = archive_read_new();
   archive_read_support_format_all(a);
   archive_read_support_filter_all(a);

   struct archive* wd = archive_write_disk_new();
   // No ARCHIVE_EXTRACT_SECURE_* flags: NOABSOLUTEPATHS would reject every
   // entry because we set an absolute destination path ourselves, and
   // SECURE_SYMLINKS rejects extraction into any path that passes through a
   // host-filesystem symlink (which our music root may well contain).
   // Path traversal is prevented entirely by the sanitisation loop below.
   archive_write_disk_set_options(wd, ARCHIVE_EXTRACT_TIME);

   auto cleanup = [&]{
      archive_read_close(a);
      archive_read_free(a);
      archive_write_close(wd);
      archive_write_free(wd);
      };

   int r = archive_read_open_memory(a, content.data(), content.size());
   if (r != ARCHIVE_OK) {
      std::cout << stamp() << "extract: open failed (" << r << "): "
                << archive_error_string(a) << std::endl;
      cleanup();
      return -1;
      }

   int count = 0, skipped = 0;
   struct archive_entry* entry;
   int hr;
   while ((hr = archive_read_next_header(a, &entry)) == ARCHIVE_OK
          || hr == ARCHIVE_WARN) {
      if (hr == ARCHIVE_WARN)
         std::cout << stamp() << "extract: header warn: "
                   << archive_error_string(a) << std::endl;

      const char* raw = archive_entry_pathname(entry);
      if (!raw) { skipped++; continue; }

      // Build a sanitised relative path by dropping any "..", ".", and "/" components.
      fs::path safe;
      for (const auto& part : fs::path(raw)) {
         auto s = part.string();
         if (s == ".." || s == "." || s == "/") continue;
         safe /= part;
         }
      if (safe.empty()) {
         std::cout << stamp() << "extract: skip (empty after sanitise): " << raw << std::endl;
         skipped++;
         continue;
         }

      fs::path target = dest_dir / safe;
      archive_entry_set_pathname(entry, target.c_str());

      // ARCHIVE_WARN (-20) means partial success; still write the data.
      int wr = archive_write_header(wd, entry);
      if (wr < ARCHIVE_WARN) {
         std::cout << stamp() << "extract: write_header failed (" << wr << ") for "
                   << target << ": " << archive_error_string(wd) << std::endl;
         skipped++;
         continue;
         }
      if (wr == ARCHIVE_WARN)
         std::cout << stamp() << "extract: write_header warn for "
                   << target << ": " << archive_error_string(wd) << std::endl;

      const void* buf; size_t sz; la_int64_t off;
      while (archive_read_data_block(a, &buf, &sz, &off) == ARCHIVE_OK)
         archive_write_data_block(wd, buf, sz, off);
      if (archive_entry_filetype(entry) == AE_IFREG)
         count++;
      archive_write_finish_entry(wd);
      }

   if (hr != ARCHIVE_EOF)
      std::cout << stamp() << "extract: read_next_header stopped (r=" << hr << "): "
                << archive_error_string(a) << std::endl;

   std::cout << stamp() << "extract: done, files=" << count
             << " skipped=" << skipped << std::endl;
   cleanup();
   return count;
   }

// Reorganise all audio files under batch_root into <artist>/<album>/ subdirs
// using embedded tag metadata. Non-audio siblings follow their audio files when
// all audio in a source dir maps to the same (artist, album) target. Empty
// directories left behind are pruned. Falls back to "Unknown Artist" /
// "Unknown Album" for files with no usable tags.
static void reorganise_by_tags(const std::filesystem::path& batch_root)
   {
   namespace fs = std::filesystem;

   static const std::set<std::string> AUDIO_EXT = {
      ".flac", ".mp3", ".ogg", ".oga", ".m4a", ".aac", ".wav", ".opus", ".wma"};
   auto is_audio = [](const fs::path& p) {
      std::string ext = p.extension().string();
      std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
      return AUDIO_EXT.count(ext) > 0;
      };
   auto sanitise = [](const std::string& s) -> std::string {
      std::string r;
      for (char c : s)
         if (c != '/' && c != '\\' && c != '\0' && c != ':' &&
             c != '*'  && c != '?' && c != '"'  && c != '<' && c != '>')
            r += c;
      while (!r.empty() && (r.front() == ' ' || r.front() == '.'))
         r.erase(r.begin());
      while (!r.empty() && (r.back()  == ' ' || r.back()  == '.'))
         r.pop_back();
      return r.empty() ? "Unknown" : r;
      };

   // Collect audio files that are NOT already in an Artist/Album subdir
   // (depth >= 2 from batch_root). Those are left in place; only flat or
   // single-level files need reorganising.
   struct AudioFile { fs::path path; std::string artist; std::string album; };
   std::vector<AudioFile> audio_files;
   std::error_code ec;
   for (auto& e : fs::recursive_directory_iterator(batch_root, ec)) {
      if (!e.is_regular_file() || !is_audio(e.path())) continue;
      auto rel   = e.path().lexically_relative(batch_root);
      int  depth = (int)std::distance(rel.begin(), rel.end()) - 1; // -1 for filename
      if (depth >= 2) continue;  // already in Artist/Album structure
      std::string artist = "Unknown Artist", album = "Unknown Album";
      TagLib::FileRef ref(e.path().c_str());
      if (!ref.isNull() && ref.tag()) {
         auto a = ref.tag()->artist().to8Bit(true);
         auto b = ref.tag()->album().to8Bit(true);
         if (!a.empty()) artist = a;
         if (!b.empty()) album  = b;
         }
      audio_files.push_back({e.path(), sanitise(artist), sanitise(album)});
      }

   if (audio_files.empty()) return;

   // Move each audio file into batch_root/<artist>/<album>/.
   // Track which source directories contributed to which (artist, album) targets
   // so sibling non-audio files (covers, .m3u, etc.) can follow.
   std::map<fs::path, std::set<std::pair<std::string, std::string>>> dir_targets;
   for (auto& af : audio_files) {
      fs::path target = batch_root / af.artist / af.album;
      fs::create_directories(target, ec);
      fs::rename(af.path, target / af.path.filename(), ec);
      if (ec)
         std::cout << stamp() << "reorganise: rename failed for "
                   << af.path << ": " << ec.message() << std::endl;
      else
         dir_targets[af.path.parent_path()].insert({af.artist, af.album});
      }

   // For source dirs that fed exactly one (artist, album) target, relocate
   // any remaining files (covers, lyrics, etc.) to the same destination.
   for (auto& [src, targets] : dir_targets) {
      if (targets.size() != 1) continue;
      auto& [artist, album] = *targets.begin();
      fs::path target = batch_root / artist / album;
      for (auto& e : fs::directory_iterator(src, ec))
         if (e.is_regular_file())
            fs::rename(e.path(), target / e.path().filename(), ec);
      }

   // Prune empty directories — collect them all first, then sort deepest-first
   // so children are removed before parents.
   std::vector<fs::path> dirs;
   for (auto& e : fs::recursive_directory_iterator(batch_root, ec))
      if (e.is_directory()) dirs.push_back(e.path());
   std::sort(dirs.begin(), dirs.end(),
             [](const fs::path& a, const fs::path& b){
                return b.string().size() < a.string().size();
                });
   for (auto& d : dirs)
      if (fs::is_empty(d, ec)) fs::remove(d, ec);
   }

// Grace period before the SSE-as-heartbeat watchdog tears down a cast
// session whose listener has gone away. Long enough to ride out a page
// reload or a brief network blip; short enough that closing the tab
// actually stops the cast.
static constexpr int CAST_IDLE_GRACE_S = 30;

GainDrive::GainDrive(const std::string& db_path,
                     const std::vector<MediaStore::Root>& roots,
                     const std::string& upload_dir,
                     bool no_scan,
                     bool debug,
                     bool flat_multi_disc,
                     const std::string& user_db_path,
                     const std::string& transcode_cache_dir,
                     int transcode_cache_mb,
                     int transcode_jobs)
	: debug_(debug), flat_multi_disc_(flat_multi_disc), upload_dir_(upload_dir),
	  store_(db_path, roots, user_db_path),
	  transcode_cache_(
	      transcode_cache_dir.empty()
	          ? std::filesystem::path(db_path).parent_path() / "transcodes"
	          : std::filesystem::path(transcode_cache_dir),
	      static_cast<int64_t>(transcode_cache_mb) * 1024 * 1024,
	      transcode_jobs > 0 ? transcode_jobs
	          : std::max(2u, std::thread::hardware_concurrency() / 2)),
	  watcher_(store_)
	{
	namespace fs = std::filesystem;
	// A cache inside any root would be rescanned and indexed, and the
	// transcodes would then appear in the library as tracks of their own.
	if (store_.path_is_within_root(
	        fs::absolute(transcode_cache_dir.empty()
	            ? fs::path(db_path).parent_path() / "transcodes"
	            : fs::path(transcode_cache_dir)).string())) {
		std::cerr << stamp()
		          << "Error: the transcode cache must not live inside "
		          << "a library root." << std::endl;
		std::exit(1);
		}
	// A cache miss now blocks its request thread for the whole transcode
	// (seconds, not milliseconds), so the default pool of 8 is too small to
	// absorb a client that pins an album and fans out downloads.  The ffmpeg
	// count is bounded separately by --transcode-jobs; this only bounds waiting.
	server_.new_task_queue = []{ return new httplib::ThreadPool(32); };

	if (!fs::exists(upload_dir_))
		fs::create_directories(upload_dir_);
	// Personal uploads live in their own root rather than a hidden directory
	// inside a library, so nothing there can be mistaken for someone's album.
	// With no uploads root configured both stay empty and the upload endpoints
	// refuse — better than silently writing into a library root.
	if (const auto* up = store_.uploads_root()) {
		users_dir_         = up->path;
		uploads_root_name_ = up->name;
		fs::create_directories(users_dir_);
		}
	else
		std::cout << stamp() << "No uploads root configured; personal uploads "
		          << "are disabled." << std::endl;

	// Normalise /rest/foo → /rest/foo.view so clients that omit the suffix still work.
	// In debug mode also strip Accept-Encoding: httplib swaps compressed bytes into
	// res.body before firing the logger, making it unreadable (cpp-httplib#1656).
	server_.set_pre_routing_handler([this](const httplib::Request& req, httplib::Response&) {
		auto& r = const_cast<httplib::Request&>(req);
		if (r.path.rfind("/rest/", 0) == 0 && r.path.find('.') == std::string::npos)
			r.path += ".view";
		if (debug_)
			r.headers.erase("Accept-Encoding");
		return httplib::Server::HandlerResponse::Unhandled;
		});

	server_.set_logger([this](const httplib::Request& req, const httplib::Response& res) {
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
		if (debug_ && !res.body.empty()) {
			auto ct = res.get_header_value("Content-Type");
			std::cout << ct << "\n";
//			if (ct == "application/json" || ct == "application/xml")
//				std::cout << res.body << "\n";
			}
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
		res.set_content(body, use_json ? "application/json" : "application/xml");
		});

	// getServerSettings / saveServerSettings — admin-only server configuration.
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

		std::string token = store_.get_setting("discogs_token");
		std::string body = subsonic_ok_json([&token](nlohmann::json& r) {
			r["serverSettings"]["discogsToken"] = token;
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

		store_.set_setting("discogs_token", qp("discogsToken"));
		std::string body = use_json ? subsonic_ok_json() : subsonic_ok();
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

	// getIndexes — all artists grouped by first letter.
	server_.Get("/rest/getIndexes.view", [this](const httplib::Request& req,
	                                             httplib::Response& res) {
		if (!check_auth(req, res, store_)) return;

		bool personal = req.get_param_value("personal") == "true";
		std::string pu = personal ? req.get_param_value("u") : "";
		// musicFolderId restricts to one root; absent means all of them,
		// which is what every existing client sends.
		auto artists = store_.get_artist_dirs(
			pu, to_int(req.get_param_value("musicFolderId"), 0));

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

	// getArtists — same artists as getIndexes but with albumCount per artist.
	server_.Get("/rest/getArtists.view", [this](const httplib::Request& req,
	                                            httplib::Response& res) {
		if (!check_auth(req, res, store_)) return;

		bool personal = req.get_param_value("personal") == "true";
		std::string pu = personal ? req.get_param_value("u") : "";
		// musicFolderId restricts to one root; absent means all of them,
		// which is what every existing client sends.
		auto artists = store_.get_artist_dirs(
			pu, to_int(req.get_param_value("musicFolderId"), 0));
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
						{"id",        sid(al.id)},
						{"parent",    sid(al.parent_id)},
						{"artistId",  sid(al.parent_id)},
						{"isDir",     true},
						{"title",     al.title},
						{"name",      al.title},
						{"artist",    al.artist},
						{"songCount", al.song_count},
						{"duration",  al.duration}
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

	// getAlbumList / getAlbumList2 — both use the same folder-based logic.
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

	// getRecentSongs — gaindrive extension; not in the OpenSubsonic spec.
	// Returns songs ordered by most recently played (per-user play_counts).
	server_.Get("/rest/getRecentSongs.view", [this](const httplib::Request& req,
	                                                httplib::Response& res) {
		if (!check_auth(req, res, store_)) return;

		auto qp = [&](const std::string& k, const std::string& def = "") {
			auto it = req.params.find(k);
			return it != req.params.end() ? it->second : def;
			};

		std::string user = qp("u");
		int size   = std::min(500, std::max(1, std::stoi(qp("size",   "50"))));
		int offset = std::max(0,               std::stoi(qp("offset", "0")));

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

	// getArtistInfo / getArtistInfo2 — MusicBrainz lookup, result cached in DB.
	// Both endpoints share identical logic; only the response key name differs.
	server_.Get("/rest/getArtistInfo.view", [this](const httplib::Request& req,
	                                               httplib::Response& res) {
		if (!check_auth(req, res, store_)) return;
		handle_artist_info(req, res, store_, "artistInfo");
		});
	server_.Get("/rest/getArtistInfo2.view", [this](const httplib::Request& req,
	                                                httplib::Response& res) {
		if (!check_auth(req, res, store_)) return;
		handle_artist_info(req, res, store_, "artistInfo2");
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
			// For artist folder IDs: trigger MusicBrainz/Wikipedia lookup if needed
			// and serve the portrait. Never fall back to album cover art.
			std::string name = store_.get_folder_name(folder_id);
			if (!name.empty()) {
				auto artist_info = resolve_artist_info(folder_id, name, store_);
				if (!artist_info.image_url.empty()) {
					serve_artist_portrait(res, artist_info.image_url, folder_id);
					return;
					}
				}
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
			std::cout << stamp() << "getCoverArt: refusing path outside every root: "
			          << path << std::endl;
			res.status = 403;
			return;
			}

		namespace fs = std::filesystem;
		auto size_it = req.params.find("size");

		// Revalidate rather than trust the cache.  A cover URL is identified
		// only by `id`, and folders.id is a rowid that is NOT stable across a
		// rescan — which is exactly why stars and playlists key on paths
		// instead.  Without a validator a browser caches the image
		// heuristically and indefinitely, so after a rebuild reassigns ids it
		// keeps showing the previous album's art with no way to notice.
		// "no-cache" means "keep it, but check first", so this stays fast.
		std::error_code mec, sec;
		auto  mtime = fs::last_write_time(path, mec);
		auto  fsize = fs::file_size(path, sec);
		// Each stat gets its own error_code, and both are resolved before the
		// string is built: sharing one would read and write it within a single
		// unsequenced expression.
		long long mstamp = mec ? 0 : static_cast<long long>(
			mtime.time_since_epoch().count());
		std::string etag = "\"" + std::to_string(mstamp)
		    + "-" + std::to_string(sec ? 0 : fsize)
		    + "-" + (size_it != req.params.end() ? size_it->second
		                                         : std::string("full"))
		    + "-" + (idx_it != req.params.end() ? idx_it->second
		                                        : std::string("0")) + "\"";
		res.set_header("Cache-Control", "no-cache");
		res.set_header("ETag", etag);
		if (req.get_header_value("If-None-Match") == etag) {
			res.status = 304;
			return;
			}

		if (size_it != req.params.end()) {
			serve_cover_scaled(res, path, to_int(size_it->second, 200));
			return;
			}

		// Serve the full-size image directly.
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
			std::cout << stamp() << "getAlbumTexts: refusing path outside every root: "
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
			std::cout << stamp() << "getAlbumText: refusing path outside every root: "
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

		auto song = store_.get_song(to_int(it->second, -1));
		if (!song) {
			res.set_content(subsonic_error(70, "Song not found."), "application/xml");
			return;
			}

		// Compose-and-validate the absolute song path once. Streamer reads from
		// it (via std::ifstream and ffmpeg argv) — refuse anything outside
		// the configured roots before handing it off.
		std::string song_abs = store_.abs_path(song->path);
		if (!store_.path_is_within_root(song_abs)) {
			std::cout << stamp() << "stream: refusing path outside every root: "
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
			float cast_offset = to_it != req.params.end()
			    ? to_float(to_it->second, 0.0f) : 0.0f;
			cast_manager_.load(url, std::string(codec_to_mime(song->codec)), cast_offset, song->duration);
			res.status = 204;
			return;
			}

		auto qp = [&](const std::string& k, const std::string& def = "") {
			auto it2 = req.params.find(k);
			return it2 != req.params.end() ? it2->second : def;
			};

		int         max_bitrate = to_int(qp("maxBitRate"), 0);
		// Enforce the user account's max_bitrate as a ceiling (0 = unlimited).
		// Cast requests authenticate via token and have no 'u' param; skip for those.
		if (!cast_authed) {
			int acct_max = request_max_bitrate(req, store_);
			if (acct_max > 0 && (max_bitrate == 0 || max_bitrate > acct_max))
				max_bitrate = acct_max;
			}
		std::string format      = qp("format");
		// Reject a format we have no encoder for here, where the Subsonic error
		// helpers live.  Letting it reach ffmpeg produced a 200 with an empty
		// body, which every client reports as a corrupt file rather than as a
		// bad request.
		if (!format.empty() && format != "raw") {
			auto t = target_for(format);
			if (!t || t->encoder.empty()) {
				bool as_json = fmt_of(req) == "json";
				std::string msg = "Unsupported format: " + format + ".";
				res.set_content(as_json ? subsonic_error_json(10, msg.c_str())
				                        : subsonic_error(10, msg.c_str()),
				                as_json ? "application/json" : "application/xml");
				return;
				}
			}
		// The Chromecast sometimes probes the stream URL with timeOffset stripped.
		// Always use the authoritative offset stored at castLoad time for cast
		// requests so the probe and the real request both start at the right position.
		int         time_offset = cast_authed
		    ? static_cast<int>(last_cast_offset_)
		    : to_int(qp("timeOffset"), 0);

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
			auto probe_si = streamer_song(*song, song_abs,
			                              std::min(song->file_size,
			                                       (int64_t)32768));
			Streamer::serve(req, res, probe_si, transcode_cache_, 0, "", 0, true, {});
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

		auto si = streamer_song(*song, song_abs);

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

		if (!cast_authed)
			std::cout << stamp() << "stream: id=" << it->second
			          << " codec=" << song->codec
			          << " size=" << song->file_size
			          << " duration=" << song->duration
			          << (time_offset > 0 ? " offset=" + std::to_string(time_offset) : "")
			          << (!format.empty() ? " fmt=" + format : "")
			          << std::endl;
		bool estimate_length = qp("estimateContentLength") == "true";
		// Video-only per the spec, and ignored for audio by Streamer::serve().
		// `duration` is what makes an HLS segment a segment: hls.m3u8 points
		// every segment back here with a timeOffset and a length.
		std::string video_size = qp("size");
		int         seg_dur    = to_int(qp("duration"), 0);
		Streamer::serve(req, res, si, transcode_cache_, max_bitrate, format,
		                time_offset, cast_authed, std::move(get_pos),
		                estimate_length, video_size, seg_dur);
		});

	// download — the original file, never transcoded and never bitrate-capped.
	// The per-user max_bitrate is deliberately not consulted: "download" is
	// defined by the API as the original media data, and a capped download
	// would silently hand the user a different file than the one they asked
	// for.  Only song ids are supported; zipping a folder is out of scope.
	server_.Get("/rest/download.view", [this](const httplib::Request& req,
	                                           httplib::Response& res) {
		if (!check_auth(req, res, store_)) return;
		bool use_json = (fmt_of(req) == "json");
		auto err = [&](int code, const char* msg) {
			res.set_content(use_json ? subsonic_error_json(code, msg)
			                         : subsonic_error(code, msg),
			                use_json ? "application/json" : "application/xml");
			};

		auto it = req.params.find("id");
		if (it == req.params.end()) {
			err(10, "Required parameter missing: id."); return;
			}
		auto song = store_.get_song(to_int(it->second, -1));
		if (!song) { err(70, "Song not found."); return; }

		std::string song_abs = store_.abs_path(song->path);
		if (!store_.path_is_within_root(song_abs)) {
			std::cout << stamp() << "download: refusing path outside every root: "
			          << song_abs << std::endl;
			res.status = 403;
			return;
			}

		std::string name = std::filesystem::path(song->path).filename().string();
		// Two spellings of the filename: a sanitised ASCII one for clients that
		// only read the bare parameter, and the RFC 5987 form for the rest.
		// A quote or newline left in the ASCII form would let a filename break
		// out of the header.
		std::string ascii;
		for (unsigned char c : name)
			ascii += (c < 0x20 || c == 0x7f || c == '"' || c == '\\'
			          || c >= 0x80) ? '_' : static_cast<char>(c);
		std::ostringstream enc;
		enc << std::hex << std::uppercase << std::setfill('0');
		for (unsigned char c : name) {
			if (isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~')
				enc << static_cast<char>(c);
			else
				enc << '%' << std::setw(2) << static_cast<int>(c);
			}
		res.set_header("Content-Disposition",
		               "attachment; filename=\"" + ascii + "\"; "
		               "filename*=UTF-8''" + enc.str());

		std::cout << stamp() << "download: id=" << it->second
		          << " path=" << song->path
		          << " size=" << song->file_size << std::endl;

		auto si = streamer_song(*song, song_abs);
		Streamer::serve_raw(req, res, si);
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
					{"id",        sid(al.id)},
					{"parent",    sid(al.parent_id)},
					{"artistId",  sid(al.parent_id)},
					{"name",      al.title},
					{"artist",    al.artist},
					{"songCount", al.song_count},
					{"duration",  al.duration},
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

	server_.Get("/rest/getAlbumInfo2.view", [this](const httplib::Request& req,
	                                               httplib::Response& res) {
		if (!check_auth(req, res, store_)) return;
		handle_album_info(req, res, store_);
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
		res.set_content(body, use_json ? "application/json" : "application/xml");
		});

	// getVideos — every video in the library, as Child entries.  Videos share
	// the songs table with audio, so this is the ordinary song serialiser with
	// a different envelope key; isVideo and type are derived from the codec.
	server_.Get("/rest/getVideos.view", [this](const httplib::Request& req,
	                                            httplib::Response& res) {
		if (!check_auth(req, res, store_)) return;
		bool use_json = (fmt_of(req) == "json");
		auto videos   = store_.get_videos();
		int  mbr      = request_max_bitrate(req, store_);

		std::string body;
		if (use_json)
			body = subsonic_ok_json([&videos, mbr](nlohmann::json& r) {
				nlohmann::json entries = nlohmann::json::array();
				for (auto& v : videos)
					entries.push_back(song_entry_json(v, mbr));
				r["videos"] = {{ "video", entries }};
				});
		else
			body = subsonic_ok([&videos, mbr](XMLDocument& doc, XMLElement* root) {
				auto* el = doc.NewElement("videos");
				for (auto& v : videos)
					el->InsertEndChild(song_entry_xml(doc, v, "video", mbr));
				root->InsertEndChild(el);
				});
		res.set_content(body, use_json ? "application/json" : "application/xml");
		});

	// getVideoInfo — the subtitle and audio tracks inside one video file.
	// Runs ffprobe per call rather than caching: it is a per-playback lookup,
	// not a browse path, and a stale track list is worse than a slow one.
	// No <conversion> child is emitted — nothing pre-transcodes today, and
	// advertising a conversion that does not exist is worse than silence.
	server_.Get("/rest/getVideoInfo.view", [this](const httplib::Request& req,
	                                               httplib::Response& res) {
		if (!check_auth(req, res, store_)) return;
		bool use_json = (fmt_of(req) == "json");
		auto err = [&](int code, const char* msg) {
			res.set_content(use_json ? subsonic_error_json(code, msg)
			                         : subsonic_error(code, msg),
			                use_json ? "application/json" : "application/xml");
			};

		auto it = req.params.find("id");
		if (it == req.params.end()) {
			err(10, "Required parameter missing: id."); return;
			}
		int  song_id = to_int(it->second, -1);
		auto song    = store_.get_song(song_id);
		if (!song || !song->is_video) { err(70, "Video not found."); return; }

		auto streams = store_.get_video_streams(song_id);

		std::string body;
		if (use_json)
			body = subsonic_ok_json([&](nlohmann::json& r) {
				nlohmann::json caps = nlohmann::json::array();
				for (auto& c : streams.captions)
					caps.push_back({ {"id",   sid(c.index)},
					                 {"name", c.title.empty() ? c.language
					                                          : c.title} });
				nlohmann::json tracks = nlohmann::json::array();
				for (auto& a : streams.audio_tracks)
					tracks.push_back({ {"id",           sid(a.index)},
					                   {"name",         a.title},
					                   {"languageCode", a.language} });
				r["videoInfo"] = {
					{"id",         sid(song_id)},
					{"captions",   caps},
					{"audioTrack", tracks}
					};
				});
		else
			body = subsonic_ok([&](XMLDocument& doc, XMLElement* root) {
				auto* vi = doc.NewElement("videoInfo");
				vi->SetAttribute("id", song_id);
				for (auto& c : streams.captions) {
					auto* el = doc.NewElement("captions");
					el->SetAttribute("id",   c.index);
					el->SetAttribute("name",
						(c.title.empty() ? c.language : c.title).c_str());
					vi->InsertEndChild(el);
					}
				for (auto& a : streams.audio_tracks) {
					auto* el = doc.NewElement("audioTrack");
					el->SetAttribute("id",           a.index);
					el->SetAttribute("name",         a.title.c_str());
					el->SetAttribute("languageCode", a.language.c_str());
					vi->InsertEndChild(el);
					}
				root->InsertEndChild(vi);
				});
		res.set_content(body, use_json ? "application/json" : "application/xml");
		});

	// getCaptions — WebVTT for one subtitle track.  Returns the file itself,
	// not a Subsonic envelope, which is what the spec asks for.  `format` is
	// accepted and ignored: WebVTT is what a <track> element can consume, and
	// handing back SRT would only push the conversion onto the client.
	server_.Get("/rest/getCaptions.view", [this](const httplib::Request& req,
	                                              httplib::Response& res) {
		if (!check_auth(req, res, store_)) return;
		bool use_json = (fmt_of(req) == "json");

		auto it = req.params.find("id");
		if (it == req.params.end()) {
			res.set_content(use_json
			                ? subsonic_error_json(10, "Required parameter missing: id.")
			                : subsonic_error(10, "Required parameter missing: id."),
			                use_json ? "application/json" : "application/xml");
			return;
			}
		// captionId selects an embedded stream; absent means the sidecar file.
		auto cid_it = req.params.find("captionId");
		int  index  = cid_it != req.params.end()
		    ? to_int(cid_it->second, -1) : -1;

		auto vtt = store_.get_captions_vtt(to_int(it->second, -1), index);
		if (vtt.empty()) {
			std::cout << stamp() << "getCaptions: nothing for id=" << it->second
			          << " captionId=" << index << std::endl;
			res.status = 404;
			return;
			}
		res.set_content(vtt, "text/vtt");
		});

	// hls.m3u8 — a playlist computed from the stored duration.  Deliberately
	// stateless: no segment directory, no session, no temp files.  Every
	// segment URL is an ordinary stream.view transcode bounded by timeOffset
	// and duration, which is exactly how Subsonic does it.  Nothing here needs
	// cleaning up if a client walks away mid-playlist.
	server_.Get("/rest/hls.m3u8", [this](const httplib::Request& req,
	                                      httplib::Response& res) {
		if (!check_auth(req, res, store_)) return;

		auto it = req.params.find("id");
		if (it == req.params.end()) {
			res.set_content(subsonic_error(10, "Required parameter missing: id."),
			                "application/xml");
			return;
			}
		auto song = store_.get_song(to_int(it->second, -1));
		if (!song || !song->is_video) {
			res.set_content(subsonic_error(70, "Video not found."),
			                "application/xml");
			return;
			}

		auto qp = [&](const std::string& k, const std::string& def = "") {
			auto it2 = req.params.find(k);
			return it2 != req.params.end() ? it2->second : def;
			};
		// bitRate is the spec's spelling here; it may carry an "@WxH" suffix
		// (e.g. "1000@640x480") naming the frame size for that variant.
		std::string bitrate = qp("bitRate");
		std::string size;
		if (auto at = bitrate.find('@'); at != std::string::npos) {
			size    = bitrate.substr(at + 1);
			bitrate = bitrate.substr(0, at);
			}

		const int SEGMENT = 10;
		int total = static_cast<int>(song->duration);
		if (total <= 0) {
			res.set_content(subsonic_error(70, "Video has no known duration."),
			                "application/xml");
			return;
			}

		// Credentials ride along on every segment URL: the player fetches the
		// segments itself and carries none of this request's context.  Only
		// the parameters actually present are echoed — an empty p= alongside
		// t=/s= would send check_auth down the password branch with a blank
		// password and fail every segment.
		std::string auth;
		for (const char* k : { "u", "p", "t", "s", "c" }) {
			auto v = qp(k);
			if (!v.empty())
				auth += "&" + std::string(k) + "=" + url_encode(v);
			}
		auth += "&v=" + std::string(SUBSONIC_VER);

		std::ostringstream m3u;
		m3u << "#EXTM3U\n"
		    << "#EXT-X-VERSION:3\n"
		    << "#EXT-X-TARGETDURATION:" << SEGMENT << "\n"
		    << "#EXT-X-MEDIA-SEQUENCE:0\n"
		    << "#EXT-X-PLAYLIST-TYPE:VOD\n";
		for (int off = 0; off < total; off += SEGMENT) {
			int len = std::min(SEGMENT, total - off);
			m3u << "#EXTINF:" << len << ".0,\n"
			    << "stream.view?id=" << it->second
			    << "&timeOffset=" << off
			    << "&duration="   << len;
			if (!bitrate.empty()) m3u << "&maxBitRate=" << bitrate;
			if (!size.empty())    m3u << "&size=" << size;
			m3u << auth << "\n";
			}
		m3u << "#EXT-X-ENDLIST\n";

		std::cout << stamp() << "hls: id=" << it->second
		          << " duration=" << total
		          << " segments=" << ((total + SEGMENT - 1) / SEGMENT)
		          << std::endl;
		res.set_content(m3u.str(), "application/vnd.apple.mpegurl");
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

		bool personal = qp("personal") == "true";
		std::string pu = personal ? qp("u") : "";
		auto sr = store_.search(query,
		                        artist_count, artist_offset,
		                        album_count,  album_offset,
		                        song_count,   song_offset,
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
					float pos = cast_manager_.last_known_time();
					std::string url = proto + "://" + host + "/rest/stream.view"
					                + "?id=" + last_cast_song_id_
					                + "&castToken=" + cast_manager_.token();
					last_cast_offset_ = 0.0f;
					cast_manager_.load(url, std::string(codec_to_mime(song->codec)),
					                   pos > 0.5f ? pos : 0.0f,
					                   song->duration);
					}
				}
			}
		else if (action == "seek") {
			auto ti = req.params.find("time");
			if (ti != req.params.end())
				cast_manager_.cast_seek(to_float(ti->second, 0.0f));
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

		auto song = store_.get_song(to_int(it->second, -1));
		if (!song) { err(70, "Song not found."); return; }

		std::string host  = req.get_header_value("Host");
		if (host.empty()) host = "localhost";
		std::string proto = req.get_header_value("X-Forwarded-Proto");
		if (proto.empty()) proto = "http";
		std::string url = proto + "://" + host + "/rest/stream.view"
		                + "?id=" + it->second
		                + "&castToken=" + cast_manager_.token();
		auto to_it = req.params.find("timeOffset");
		float cast_offset = to_it != req.params.end()
		    ? to_float(to_it->second, 0.0f) : 0.0f;
		last_cast_song_id_ = it->second;
		last_cast_offset_ = 0.0f;
		cast_manager_.load(url, std::string(codec_to_mime(song->codec)),
		                   cast_offset, song->duration);
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
				std::cout << stamp() << "updateSong: refusing path outside every root: "
				          << song_abs << std::endl;
				err(0, "Refusing to write outside the configured roots.");
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
			std::cout << stamp() << "setCoverArt: refusing path outside every root: "
			          << cover_abs.string() << std::endl;
			err(0, "Refusing to write outside the configured roots.");
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

	// Upload a music archive (zip / tar / tar.gz / tgz) and extract it into
	// the calling user's personal folder under <uploads root>/<username>/.
	server_.Post("/upload", [this](const httplib::Request& req, httplib::Response& res) {
		if (!check_auth(req, res, store_)) return;

		auto json_err = [&](const std::string& msg) {
			nlohmann::json j;
			j["status"]  = "error";
			j["message"] = msg;
			res.status = 400;
			res.set_content(j.dump(), "application/json");
			};

		// Refuse rather than fall back to a library root: an upload landing in
		// a shared library would be scanned as somebody's album.
		if (users_dir_.empty()) {
			json_err("No uploads root is configured on this server.");
			return;
			}

		// Require upload_allowed (or admin); capture the username for the dest path.
		std::string uname;
		{
		auto it = req.params.find("u");
		uname = (it != req.params.end()) ? it->second : std::string{};
		auto ui = store_.get_user(uname);
		if (!ui || (!ui->upload_allowed && !ui->is_admin)) {
			json_err("User is not authorized to upload.");
			return;
			}
		}

		if (!req.has_file("file")) { json_err("Missing file part."); return; }
		const auto& fp = req.get_file_value("file");

		// Validate extension.
		const std::string& name = fp.filename;
		bool ok = name.ends_with(".zip")
		       || name.ends_with(".tar")
		       || name.ends_with(".tar.gz")
		       || name.ends_with(".tgz");
		if (!ok) { json_err("Unsupported file type. Use zip, tar, tar.gz, or tgz."); return; }

		namespace fs = std::filesystem;

		// Each upload lands in its own UUID subdirectory so that messy zip
		// structures (missing artist/album dirs) are always isolated and the
		// scanner has a stable "artist-level" root to work from.
		std::string uuid = make_uuid();
		fs::path dest = fs::path(users_dir_) / uname / uuid;
		fs::create_directories(dest);

		std::cout << stamp() << "Upload: extracting " << name
		          << " (" << fp.content.size() << " bytes)"
		          << " for user " << uname
		          << " into " << dest << std::endl;

		int n = extract_archive_to_dir(fp.content, dest);
		if (n < 0) { json_err("Failed to open archive."); return; }

		std::cout << stamp() << "Upload: extracted " << n << " file(s) to " << dest << std::endl;

		// Reorganise extracted files into <artist>/<album>/ dirs based on tags.
		reorganise_by_tags(dest);
		std::cout << stamp() << "Upload: reorganised by tags under " << dest << std::endl;

		// Scan each artist dir inside the batch individually so that artist names
		// (not the UUID) appear as the top-level entries in personal mode.
		std::string rel_batch = uploads_root_name_ + "/" + uname + "/" + uuid;
		{
		std::set<std::string> to_scan;
		std::error_code ec;
		for (auto& e : fs::directory_iterator(dest, ec))
			if (e.is_directory())
				to_scan.insert(rel_batch + "/" + e.path().filename().string());
		if (!to_scan.empty())
			std::thread([this, to_scan]{ store_.scan_dirs(to_scan); }).detach();
		}

		nlohmann::json j;
		j["status"] = "ok";
		j["files"]  = n;
		j["batch"]  = rel_batch;
		res.set_content(j.dump(), "application/json");
		});

	// promoteAlbum — move a personal album into the shared library (admin only).
	// Param: id (album folder_id). The album must live under .users/<username>/.
	server_.Get("/rest/promoteAlbum.view", [this](const httplib::Request& req,
	                                               httplib::Response& res) {
		if (!check_auth(req, res, store_)) return;
		bool use_json = (fmt_of(req) == "json");

		auto err = [&](int code, const char* msg) {
			res.set_content(use_json ? subsonic_error_json(code, msg)
			                         : subsonic_error(code, msg),
			                use_json ? "application/json" : "application/xml");
			};

		// Admin only.
		{
		auto it = req.params.find("u");
		auto ui = (it != req.params.end()) ? store_.get_user(it->second) : std::nullopt;
		if (!ui || !ui->is_admin) { err(50, "Promote requires admin role."); return; }
		}

		auto id_it = req.params.find("id");
		if (id_it == req.params.end()) { err(10, "Missing parameter: id."); return; }
		int folder_id = std::stoi(id_it->second);

		auto rel_opt = store_.album_folder_path_by_id(folder_id);
		if (!rel_opt) { err(70, "Item not found."); return; }
		const std::string& item_rel = *rel_opt;

		// Expect "<uploads root>/<username>/<uuid>/<artist>/<album>" — five parts.
		namespace fs = std::filesystem;
		fs::path rel_p(item_rel);
		std::vector<std::string> parts;
		for (auto& c : rel_p) parts.push_back(c.string());
		if (parts.size() != 5 || uploads_root_name_.empty()
		        || parts[0] != uploads_root_name_) {
			err(0, "Item is not in a personal library folder.");
			return;
			}
		const std::string& uname_from_path = parts[1];
		const std::string& batch_uuid      = parts[2];
		const std::string& artist_name     = parts[3];
		const std::string& album_name      = parts[4];

		// Album lands under the artist dir in the main library.  With several
		// artist roots configured there is nothing in a personal upload that
		// says which one it belongs in, so it goes to the first one declared —
		// the order the operator wrote them in is the only signal available.
		std::string lib_root;
		for (const auto& r : store_.roots())
			if (r.type == "artists") { lib_root = r.name; break; }
		if (lib_root.empty()) {
			err(0, "No artist library root is configured to promote into.");
			return;
			}
		std::string artist_rel = lib_root + "/" + artist_name;
		std::string target_rel = artist_rel + "/" + album_name;

		fs::path abs_src    = store_.abs_path(item_rel);
		fs::path abs_target = store_.abs_path(target_rel);

		std::error_code ec;
		// Create the artist dir in the main library if it doesn't exist yet.
		fs::create_directories(store_.abs_path(artist_rel), ec);

		if (fs::exists(abs_target)) {
			err(0, "An album with that name already exists for that artist.");
			return;
			}

		fs::rename(abs_src, abs_target, ec);
		if (ec) {
			// Cross-device: fall back to recursive copy then remove.
			fs::copy(abs_src, abs_target, fs::copy_options::recursive, ec);
			if (ec) { err(0, ("Failed to move item: " + ec.message()).c_str()); return; }
			fs::remove_all(abs_src, ec);
			}

		std::cout << stamp() << "Promote: moved " << item_rel
		          << " → " << target_rel << std::endl;

		// If the personal artist dir is now empty, remove it so that
		// scan_artist_dir treats it as gone and prunes its folder row.
		std::string personal_artist_rel =
			uploads_root_name_ + "/" + uname_from_path + "/" + batch_uuid
			+ "/" + artist_name;
		fs::path personal_artist_abs = store_.abs_path(personal_artist_rel);
		if (fs::is_directory(personal_artist_abs, ec) && fs::is_empty(personal_artist_abs, ec))
			fs::remove(personal_artist_abs, ec);

		// Rescan synchronously: only two artist dirs, so it's fast, and doing it
		// before the response ensures the client sees a consistent DB immediately.
		store_.scan_dirs({artist_rel, personal_artist_rel});

		res.set_content(use_json ? subsonic_ok_json() : subsonic_ok(),
		                use_json ? "application/json" : "application/xml");
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

void GainDrive::serve_artist_portrait(httplib::Response& res,
                                      const std::string& image_url,
                                      int folder_id)
	{
	{
	std::lock_guard<std::mutex> lk(artist_img_cache_mu_);
	auto it = artist_img_cache_.find(folder_id);
	if (it != artist_img_cache_.end()) {
		auto& [ct, body] = it->second;
		res.set_content(body.c_str(), body.size(), ct.c_str());
		return;
		}
	}

	// Parse https://host/path
	const std::string prefix = "https://";
	if (image_url.rfind(prefix, 0) != 0) { res.status = 404; return; }
	auto rest     = image_url.substr(prefix.size());
	auto slash    = rest.find('/');
	if (slash == std::string::npos) { res.status = 404; return; }
	std::string host = rest.substr(0, slash);
	std::string path = rest.substr(slash);

	httplib::SSLClient cli(host);
	cli.set_default_headers({
		{"User-Agent", "GainDrive/0.1 (https://github.com/kpeeters/gaindrive)"}
		});
	auto r = cli.Get(path.c_str());
	if (!r || r->status != 200) { res.status = 404; return; }

	std::string ct = r->get_header_value("Content-Type");
	if (ct.empty()) ct = "image/jpeg";

	{
	std::lock_guard<std::mutex> lk(artist_img_cache_mu_);
	artist_img_cache_.emplace(folder_id, std::make_pair(ct, r->body));
	}
	res.set_content(r->body.c_str(), r->body.size(), ct.c_str());
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
