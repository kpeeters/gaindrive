#include "gaindrive.hh"
#include "subsonic.hh"
#include "authz.hh"
#include "stamp.hh"
#include "apientry.hh"
#include "textutil.hh"
#include "artistmatch.hh"
#include "netaddr.hh"
#include "batchtools.hh"
#include "imagescale.hh"
#include "codecs.hh"
#include "untrusted.hh"
#include "chapters.hh"

#include <algorithm>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <set>
#include <vector>

#include <archive.h>
#include <archive_entry.h>
#include <taglib/fileref.h>
#include <taglib/tag.h>
#include <tpropertymap.h>

#include <iostream>

#include <tinyxml2.h>
#include <nlohmann/json.hpp>

using namespace tinyxml2;

void GainDrive::routes_edit()
	{
	// updateSong — update title and/or track number for a single song.
	//
	// For audio the file is authoritative: the tags are written first and the
	// database only mirrors them.  For video there is no tag worth writing —
	// the scanner reads a video's title, year and episode number from its
	// filename and never opens it with TagLib — so the edit is recorded in the
	// client DB and re-applied by every scan instead.  Either way the music DB
	// stays a cache that a full rescan can rebuild.

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
		int song_id = to_int(it->second, -1);

		std::optional<std::string> title;
		std::optional<int> track_number;
		std::optional<int> year;
		std::optional<int> disc_number;
		// Typed by a person and stored raw until now: invalid UTF-8 in
		// songs.title made every later JSON response holding the song throw
		// out of dump() — a permanent 500 — and a C0 byte makes the XML
		// envelope unparseable for every conformant client.
		if (req.params.count("title")) title        = clean_name(req.params.find("title")->second);
		if (req.params.count("track")) track_number = to_int(req.params.find("track")->second, 0);
		if (req.params.count("year"))  year         = to_int(req.params.find("year")->second, 0);
		if (req.params.count("disc"))  disc_number  = to_int(req.params.find("disc")->second, 0);

		// Resolve the file path before touching anything.
		auto song = store_.get_song(song_id);
		if (!song) { err(70, "Song not found."); return; }

		// This rewrites a tag on disk, so it is a library modification and not
		// merely a read — check_auth alone would let any account retag any
		// file in any root by guessing an id.
		if (!check_item_write_perm(req, res, store_, uploads_root_name_,
		                           song->path, use_json)) return;

		if (song->is_video) {
			// Record it where a rescan can find it again.  Note what is
			// deliberately *not* done: TagLib can write an .mp4, but nothing
			// ever reads that tag back — read_song_metadata() returns after
			// the ffprobe branch — so writing it would leave two copies of one
			// fact, and the unread copy would be the one on disk.
			store_.set_song_meta_override(song->path, title, track_number,
			                               year, disc_number);
			}
		else {
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
			}

		// Persisted successfully — now mirror the change in the database.
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
		int folder_id = to_int(it->second, -1);

		// Resolved and permission-checked before anything is fetched: this
		// writes cover.jpg into a library folder and, with a url=, makes the
		// server issue an outbound request. Neither should happen for a caller
		// who is not allowed to change this folder in the first place.
		std::string folder_rel = store_.get_folder_path(folder_id);
		if (folder_rel.empty()) { err(70, "Album folder not found."); return; }
		if (!check_item_write_perm(req, res, store_, uploads_root_name_,
		                           folder_rel, use_json)) return;

		std::string bytes;
		std::string url;
		// Three spellings, because a url= is a string and a client may send it
		// any of these ways. The first is the one that matters: a multipart part
		// carrying no filename is a *text field*, and httplib files those under
		// form.fields. Before 0.54.1 every part landed in form.files whatever it
		// was, so has_file("url") alone used to be enough — the bump turned the
		// web client's FormData.append('url', …) into "Required parameter
		// missing".
		if (req.form.has_field("url"))     url = req.form.get_field("url");
		else if (req.form.has_file("url")) url = req.form.get_file("url").content;
		else if (req.has_param("url"))     url = req.get_param_value("url");

		if (req.form.has_file("file")) {
			// A reference into the form map, not get_file(), which returns a
			// copy — this part can be a whole image.
			const auto& fp = req.form.files.find("file")->second;
			if (fp.content_type.rfind("image/", 0) != 0) {
				err(0, "Uploaded file must be an image."); return;
				}
			bytes = fp.content;
			}
		else if (!url.empty()) {
			// Redirects are followed by hand rather than with
			// set_follow_location(true), because the check below has to run
			// again for each hop: a public URL that 302s to 127.0.0.1 is the
			// ordinary way an address check that only looks at what was typed
			// is defeated.
			std::string next = url;
			httplib::Result r;
			for (int hop = 0; ; ++hop) {
				if (hop > MAX_COVER_REDIRECTS) {
					err(0, "Too many redirects."); return;
					}
				bool https = next.rfind("https://", 0) == 0;
				bool http  = next.rfind("http://",  0) == 0;
				if (!https && !http) { err(0, "URL must be http(s)."); return; }
				size_t scheme_end = https ? 8 : 7;
				size_t slash = next.find('/', scheme_end);
				std::string host = next.substr(scheme_end, slash == std::string::npos
				                               ? std::string::npos : slash - scheme_end);
				std::string path = (slash == std::string::npos) ? "/" : next.substr(slash);

				// One message for "would not resolve" and "resolves somewhere
				// private", deliberately: the difference is exactly what makes
				// this endpoint a port scanner otherwise.
				if (!host_is_global(host)) {
					std::cout << stamp() << "setCoverArt: refusing non-global host: "
					          << host << std::endl;
					err(0, "Failed to fetch URL."); return;
					}

				auto fetch = [&](auto& cli) {
					cli.set_follow_location(false);
					cli.set_connection_timeout(5);
					cli.set_read_timeout(15);
					cli.set_default_headers({
						{"User-Agent", USER_AGENT}
						});
					return cli.Get(path.c_str());
					};
				auto [hname, hport] = split_host_port(host, https ? 443 : 80);
				if (https) { httplib::SSLClient cli(hname, hport); r = fetch(cli); }
				else       { httplib::Client    cli(hname, hport); r = fetch(cli); }
				if (!r) { err(0, "Failed to fetch URL."); return; }
				if (r->status == 301 || r->status == 302 || r->status == 303
				    || r->status == 307 || r->status == 308) {
					std::string loc = r->get_header_value("Location");
					if (loc.empty()) { err(0, "Failed to fetch URL."); return; }
					next = loc;
					continue;
					}
				break;
				}
			if (r->status != 200) { err(0, "Failed to fetch URL."); return; }
			// An arbitrary third-party URL; nothing about it is bounded except
			// by us. Same reasoning as MAX_PORTRAIT_BYTES.
			if (r->body.size() > MAX_COVER_BYTES) {
				err(0, "Image at URL is too large."); return;
				}
			auto ct = r->get_header_value("Content-Type");
			if (ct.rfind("image/", 0) != 0) {
				err(0, "URL did not return an image."); return;
				}
			bytes = std::move(r->body);
			}
		else {
			err(10, "Required parameter missing: file or url."); return;
			}

		// The filename follows the bytes, not the claim.  Both checks above
		// are claims — an upload's content_type is whatever the client typed
		// and a provider's Content-Type is whatever it felt like — and this
		// used to write everything as cover.jpg regardless.  That is not
		// cosmetic: the scanner indexes only .jpg/.jpeg/.png, so a WebP
		// written as cover.jpg became a cover nothing could decode.
		std::string up_mime = imagescale::sniff_mime(bytes);
		if (up_mime != "image/jpeg" && up_mime != "image/png") {
			err(0, "Unsupported image format; use JPEG or PNG.");
			return;
			}
		const bool is_png = (up_mime == "image/png");

		namespace fs = std::filesystem;
		// **A file-album's cover is the sidecar named after it**, not
		// cover.jpg inside it — there is no inside.  This is the whole point
		// of the endpoint for a loose film: under the rule where a section
		// holding loose films was itself the album, an upload here could only
		// ever set the *section's* cover, so a film TMDB had matched wrongly
		// could not be given the right poster at all. setCoverArt is the only
		// remedy for a bad match, so that gap had no way round it.
		//
		// The names are find_song_cover()'s first two, which is what makes the
		// next scan pick this up.
		std::error_code fec;
		const bool file_album =
			fs::is_regular_file(store_.abs_path(folder_rel), fec);
		auto beside = [&](const char* suffix) {
			fs::path p(folder_rel);
			p.replace_extension("");
			return p.string() + suffix;
			};
		std::string cover_rel = file_album
		    ? beside(is_png ? ".png" : ".jpg")
		    : (fs::path(folder_rel) / (is_png ? "cover.png" : "cover.jpg")).string();
		fs::path cover_abs = fs::path(store_.abs_path(cover_rel));
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

		// find_cover() prefers cover.jpg over cover.png, so uploading a PNG
		// beside an existing cover.jpg would leave the old one winning on the
		// next scan and the upload apparently ignored.  Only the names this
		// upload outranks are removed.
		//
		// For a file-album the list is one longer, because find_song_cover()'s
		// order is .jpg, .jpeg, .png: writing the .png has to clear *both*
		// earlier spellings or the stale one keeps winning, which is the same
		// bug one rung further along.  The "-poster.*" forms are never touched
		// — they rank below all three, so they cannot win anyway.
		std::vector<std::string> outranked;
		if (file_album) {
			if (is_png) { outranked.push_back(beside(".jpg"));
			              outranked.push_back(beside(".jpeg")); }
			}
		else
			outranked.push_back(
				(fs::path(folder_rel) / (is_png ? "cover.jpg" : "cover.png")).string());
		for (auto& other_rel : outranked) {
			fs::path other_abs = fs::path(store_.abs_path(other_rel));
			std::error_code rec;
			if (store_.path_is_within_root(other_abs) && fs::exists(other_abs, rec)) {
				fs::remove(other_abs, rec);
				store_.drop_cover_thumbs(other_rel);
				cover_cache_.invalidate(other_rel);
				}
			}

		// Unchanged, and it is what closes the gap above: the manual_covers row
		// is keyed on `folder_rel`, which for a file-album is the media file's
		// own stored path — the very string lookup_video_meta() passes to
		// cover_is_manual() before it fetches a poster.  So a poster chosen
		// here now survives every later scan.
		store_.set_cover_art_path(folder_rel, cover_rel);
		cover_cache_.invalidate(cover_rel);

		res.set_content(use_json ? subsonic_ok_json() : subsonic_ok(),
		                use_json ? "application/json" : "application/xml");
		});

	// Upload a music archive (zip / tar / tar.gz / tgz) and extract it into
	// the calling user's personal folder under <uploads root>/<username>/.
	// The content-reader form, and that is the security boundary as much as a
	// convenience: httplib hands the handler control *before* reading a byte
	// of body, so auth and the upload permission are checked against the
	// query-string credentials first, and an unauthenticated multi-gigabyte
	// POST costs the server nothing. The archive then streams to a file on
	// disk rather than through req.body — the old form buffered the whole
	// body in memory (and the multipart copy doubled it), which made /upload
	// the cheapest OOM in the server.
	server_.Post("/upload", [this](const httplib::Request& req, httplib::Response& res,
	                               const httplib::ContentReader& content_reader) {
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

		// Typed names, exactly as fetchUrl takes them — the two producers are
		// the same steps around a different source of bytes, and scan_batch()
		// below has always accepted these. Blank means "keep what the archive's
		// own folders and the files' tags say", which is what every upload did
		// before this existed.
		std::string want_artist, want_album;
		{
		auto typed = [&](const char* key, std::string& out) -> bool {
			auto it = req.params.find(key);
			if (it == req.params.end()) return true;
			if (it->second.find_first_not_of(" \t") == std::string::npos) return true;
			// utf8_clean first: invalid UTF-8 reaches folders.path and then
			// dump(), which throws. Then sanitise_component, which is what
			// stops a typed "../.." being a path at all.
			out = sanitise_component(utf8_clean(it->second, 200));
			return !out.empty();
			};
		if (!typed("artist", want_artist)) {
			json_err("The artist name contains nothing usable."); return;
			}
		if (!typed("album", want_album)) {
			json_err("The album name contains nothing usable."); return;
			}
		}

		if (!req.is_multipart_form_data()) {
			json_err("Expected multipart/form-data."); return;
			}

		namespace fs = std::filesystem;

		// Each upload lands in its own UUID subdirectory so that messy zip
		// structures (missing artist/album dirs) are always isolated and the
		// scanner has a stable "artist-level" root to work from. The archive
		// itself streams to a sibling .part file — outside dest, so the
		// extraction can never mistake it for content — and is deleted
		// whatever happens.
		std::string uuid = make_uuid();
		fs::path dest = fs::path(users_dir_) / uname / uuid;
		fs::path part = fs::path(users_dir_) / uname / (uuid + ".upload.part");

		std::ofstream out;
		std::string cur_part;      // name of the multipart part being received
		std::string name;          // the file part's client-side filename
		uint64_t received = 0;
		bool too_big = false, bad_type = false, io_failed = false;

		auto type_ok = [](const std::string& n) {
			return n.ends_with(".zip") || n.ends_with(".tar")
			    || n.ends_with(".tar.gz") || n.ends_with(".tgz");
			};

		bool read_ok = content_reader(
			[&](const httplib::FormData& fd) {
				cur_part = fd.name;
				if (cur_part != "file") return true;   // other parts: ignored
				name = fd.filename;
				// Refusing here aborts the read, so a wrong extension costs
				// the client its upload time, not the server its disk.
				if (!type_ok(name)) { bad_type = true; return false; }
				std::error_code ec;
				fs::create_directories(part.parent_path(), ec);
				out.open(part, std::ios::binary | std::ios::trunc);
				if (!out) { io_failed = true; return false; }
				return true;
				},
			[&](const char* data, size_t n) {
				if (cur_part != "file") return true;
				received += n;
				if (received > MAX_REQUEST_BYTES) { too_big = true; return false; }
				out.write(data, static_cast<std::streamsize>(n));
				if (!out) { io_failed = true; return false; }
				return true;
				});
		if (out.is_open()) out.close();

		auto drop_part = [&]{ std::error_code ec; fs::remove(part, ec); };

		if (bad_type) {
			drop_part();
			json_err("Unsupported file type. Use zip, tar, tar.gz, or tgz.");
			return;
			}
		if (too_big)   { drop_part(); res.status = 413;
		                 json_err("Upload too large."); return; }
		if (io_failed) { drop_part(); res.status = 500;
		                 json_err("Could not store the upload."); return; }
		if (!read_ok)  { drop_part(); json_err("Upload was interrupted."); return; }
		if (name.empty()) { drop_part(); json_err("Missing file part."); return; }

		// Before the directory exists, so there is no window in which a batch
		// is on disk unheld. Shared because the scan that releases it runs on
		// the detached thread below, which outlives this handler; every early
		// return between here and there drops the last reference and clears
		// the marker on the way out.
		auto hold = std::make_shared<BatchHold>(dest);
		fs::create_directories(dest);

		std::cout << stamp() << "Upload: extracting " << log_safe(name)
		          << " (" << received << " bytes)"
		          << " for user " << uname
		          << " into " << dest << std::endl;

		int n = extract_archive_to_dir(part, dest);
		drop_part();
		if (n == -1) { json_err("Failed to open archive."); return; }
		// Not a message naming symlinks: with the prefix resolved the
		// remaining causes are a full disk or a permission problem, and
		// libarchive's own words are already in the log.
		if (n == -2) { json_err("Could not write the archive's contents."); return; }

		std::cout << stamp() << "Upload: extracted " << n << " file(s) to " << dest << std::endl;

		std::string rel_batch = uploads_root_name_ + "/" + uname + "/" + uuid;
		// Normalising and scanning is the same three steps for both producers —
		// an archive here, a URL fetch in fetch_worker() — so it lives in one
		// place. Detached here because this one is on an HTTP thread and the
		// client is holding a request open; the fetch worker calls it directly,
		// being a background thread already.
		std::thread([this, rel_batch, dest, hold, want_artist, want_album]{
			scan_batch(rel_batch, dest, "Unknown Artist", want_artist, want_album);
			}).detach();

		nlohmann::json j;
		j["status"] = "ok";
		j["files"]  = n;
		j["batch"]  = rel_batch;
		res.set_content(j.dump(), "application/json");
		});

	// moveAlbum — put an album somewhere, under a name.
	//
	// Params: id (album folder_id), musicFolderId, folder, album.  Everything
	// but the id is optional and every omitted part means "unchanged".
	//
	// This is one endpoint because it was always one operation.  It replaces
	// renameAlbum and promoteAlbum, which were the same handler written twice
	// — resolve the id, validate, move the directory, relocate_prefix, drop
	// the emptied source, rescan destination-then-source — differing only in
	// how the destination was spelled.  Each also had a gap the other filled:
	// a rename could not cross roots, so a film misfiled under a music root
	// could not reach a categories root without shell access, and a promote
	// could not rename, so the Move dialog could not fix a channel-derived
	// album name in the same step.
	//
	// **The destination rule** is the one place the two behaviours genuinely
	// differed, so it is a rule rather than something implicit in the code:
	//
	//  * musicFolderId given -> "<root>/<folder>/<album>", exactly two levels
	//    under that root.  The old promote, generalised.
	//  * musicFolderId omitted -> "<everything above the artist level,
	//    unchanged>/<folder>/<album>".  The old rename.  For an upload that
	//    preserves "<uploads>/<user>/<uuid>/".
	//
	// music_folder_by_id() already refuses anything that is not a browsable
	// library root, the uploads root included, so promoting *into* uploads
	// stays impossible without a test of its own.
	//
	// Note what promoteAlbum's five-component source check does **not** become
	// here: it is gone, deliberately, because with it an admin cannot move a
	// library album at all — which is the gap being closed.  What it looked
	// like it was defending is defended by the admin requirement and by
	// path_is_within_root() on the *destination*.  deleteUpload's
	// identical-looking check is a different thing and must stay: there the
	// shape *is* the boundary, since without it that endpoint is "delete any
	// folder on the server by guessing integers".
	server_.Get("/rest/moveAlbum.view", [this](const httplib::Request& req,
	                                           httplib::Response& res) {
		if (!check_auth(req, res, store_)) return;
		bool use_json = (fmt_of(req) == "json");

		auto err = [&](int code, const char* msg) {
			res.set_content(use_json ? subsonic_error_json(code, msg)
			                         : subsonic_error(code, msg),
			                use_json ? "application/json" : "application/xml");
			};

		auto id_it = req.params.find("id");
		if (id_it == req.params.end()) { err(10, "Missing parameter: id."); return; }
		int folder_id = 0;
		try { folder_id = std::stoi(id_it->second); }
		catch (...) { err(10, "Parameter id must be a number."); return; }

		auto rel_opt = store_.album_folder_path_by_id(folder_id);
		if (!rel_opt) { err(70, "Item not found."); return; }
		const std::string old_rel = *rel_opt;

		namespace fs = std::filesystem;
		std::vector<std::string> parts;
		// const auto&, not auto&: path::iterator's reference type is
		// implementation-defined. libstdc++ hands out a const path&, but libc++
		// returns a path by value, and a non-const lvalue reference cannot bind
		// to that temporary. const& works on both — it binds the reference on
		// libstdc++ and lifetime-extends the temporary on libc++.
		for (const auto& c : fs::path(old_rel)) parts.push_back(c.string());
		if (parts.size() < 2) { err(0, "Could not resolve the library path."); return; }

		// ---- Permission, first cut -------------------------------------
		//
		// The full rule — an upload user may reorganise inside their own
		// batch, naming a destination root is admin's alone — needs the
		// destination parsed and so still runs below.  What cannot wait is
		// the refusal: the shape-specific errors between here and there (is
		// it a file, how deep does it sit) were an oracle a non-admin could
		// walk over ids in other users' uploads.  A foreign upload answers
		// exactly like a missing id; a shared-library item tells a non-admin
		// no before its shape is examined.
		{
		const std::string uname = req.get_param_value("u");
		auto ui = store_.get_user(uname);
		if (!(ui && ui->is_admin)) {
			if (!item_read_allowed(req, store_, uploads_root_name_, old_rel)) {
				err(70, "Item not found."); return;
				}
			const bool own_upload = !uploads_root_name_.empty()
			                        && parts[0] == uploads_root_name_;
			if (!own_upload) {
				err(50, "Moving outside your own uploads requires admin role.");
				return;
				}
			}
		}

		// **The album may be a single media file.**  A loose file is its own
		// album, so its folder row's path names the file — and every step
		// below that assumes a directory has to be told.  The filesystem is
		// asked rather than the database because what follows is filesystem
		// work: what matters is what is actually there to move.
		std::error_code sec;
		const bool file_album =
			fs::is_regular_file(store_.abs_path(old_rel), sec);

		const std::string old_album = parts.back();
		// The name, as opposed to the on-disk leaf: for a file-album they
		// differ by the extension, which is nobody's idea of part of a title
		// and must not reach an album tag.
		const std::string src_ext =
			file_album ? fs::path(old_album).extension().string() : std::string{};
		const std::string old_album_name =
			file_album ? fs::path(old_album).stem().string() : old_album;
		// The artist level above the album.  A level-1 file-album has none of
		// its own and the root plays the part, which is what parts[0] is here
		// — the same thing ALBUM_ARTIST_ID_SQL reports as its artist.
		const std::string old_folder = parts[parts.size() - 2];

		// ---- Where it lands -------------------------------------------
		std::string new_parent_dir;   // the artist/category directory it goes in
		std::string dest_root_type;   // "artists" or "categories"

		auto mf_it = req.params.find("musicFolderId");
		const bool rerooting = (mf_it != req.params.end() && !mf_it->second.empty());

		std::string new_folder;
		if (req.params.count("folder")) {
			new_folder = sanitise_component(
				utf8_clean(req.params.find("folder")->second, 200));
			if (new_folder.empty()) {
				err(10, "The folder name contains nothing usable."); return;
				}
			}

		if (rerooting) {
			auto mf = store_.music_folder_by_id(to_int(mf_it->second, 0));
			if (!mf) { err(70, "No such library folder."); return; }
			// **folder is required when a root is named**, and that is not an
			// oversight carried over.  It was briefly optional and defaulted to
			// the source's own level-1 name, which for a fetched video is the
			// channel that published it and never a category — a guess that
			// filed documentaries under YouTube channel names.  A caller that
			// does not know where something belongs should be made to decide.
			if (new_folder.empty()) {
				err(10, "Missing parameter: folder."); return;
				}
			new_parent_dir = mf->name + "/" + new_folder;
			dest_root_type = mf->type;
			}
		else {
			// Staying put: everything above the artist level is untouched — the
			// library root, and for an upload the owner and the batch id.
			//
			// A level-1 file-album is the exception: a loose film in a root
			// used as one flat library has no artist directory, the root
			// itself plays that part, and renaming such a film in place is the
			// commonest thing this endpoint gets asked to do.  Its parent is
			// the root, which is a real directory, so there is nothing here
			// that needs one to be invented.
			if (parts.size() < 3 && !file_album) {
				err(0, "This folder is its own artist; move it to a named root, "
				       "or rename it on the server.");
				return;
				}
			if (parts.size() < 3 && !new_folder.empty() && new_folder != parts[0]) {
				err(0, "This item sits directly in a library root; name a root "
				       "with musicFolderId to file it under a folder.");
				return;
				}
			if (new_folder.empty()) new_folder = old_folder;
			if (parts.size() < 3)
				// The root is the parent; there is no level above it to keep.
				new_parent_dir = parts[0];
			else {
				std::string above;
				for (size_t i = 0; i + 2 < parts.size(); ++i)
					above += (i ? "/" : "") + parts[i];
				new_parent_dir = above + "/" + new_folder;
				}
			// The uploads root is not in get_music_folders() by design, so it
			// matches nothing here and falls through to "artists" — which is
			// what an upload's level-1 directory is.
			dest_root_type = "artists";
			for (const auto& f : store_.get_music_folders())
				if (f.name == parts[0]) { dest_root_type = f.type; break; }
			}

		std::string new_album = old_album_name;
		if (req.params.count("album")) {
			new_album = sanitise_component(
				utf8_clean(req.params.find("album")->second, 200));
			if (new_album.empty()) {
				err(10, "The album name contains nothing usable."); return;
				}
			}

		// **The on-disk leaf carries the source's extension back.**
		// sanitise_component() strips nothing but it is handed a *name*, and a
		// file-album renamed to that name alone lands on disk with no
		// extension — at which point is_media_file() stops recognising it and
		// the next scan prunes the film out of the library entirely.  A caller
		// that typed the extension is not made to type it twice.
		std::string new_leaf = new_album;
		if (file_album && !src_ext.empty()
		        && !(new_leaf.size() > src_ext.size()
		             && ends_with_ci(new_leaf, src_ext)))
			new_leaf += src_ext;

		const std::string new_rel = new_parent_dir + "/" + new_leaf;
		// Everything above the album, as it stands now.
		std::string old_parent_dir;
		for (size_t i = 0; i + 1 < parts.size(); ++i)
			old_parent_dir += (i ? "/" : "") + parts[i];

		// ---- Permission ------------------------------------------------
		//
		// Admin, except that an upload user may reorganise their own batch
		// inside itself — the case this feature exists for, since somebody who
		// has just uploaded has to be able to fix the names.  Anything that
		// names a destination root is putting something into the shared
		// library and is admin's alone.
		{
		const std::string uname = req.get_param_value("u");
		auto ui = store_.get_user(uname);
		const bool is_admin = ui && ui->is_admin;
		const bool own_upload = !uploads_root_name_.empty()
		                        && parts[0] == uploads_root_name_
		                        && parts.size() >= 5
		                        && !uname.empty() && parts[1] == uname;
		if (!is_admin && !(own_upload && !rerooting)) {
			err(50, rerooting
			        ? "Moving into the shared library requires admin role."
			        : "Moving outside your own uploads requires admin role.");
			return;
			}
		}

		if (new_rel == old_rel) {
			// Nothing to do, but answer with the ids so a client need not
			// special-case a no-op edit.
			int aid = 0, pid = 0;
			if (auto f = store_.folder_id_by_path(old_rel))        aid = *f;
			if (auto f = store_.folder_id_by_path(old_parent_dir)) pid = *f;
			res.set_content(use_json
				? subsonic_ok_json([&](nlohmann::json& r) {
					r["movedAlbum"] = {{"id",     std::to_string(aid)},
					                   {"parent", std::to_string(pid)},
					                   {"album",  new_album},
					                   {"artist", new_folder},
					                   {"tagFailures", 0}};
					})
				: subsonic_ok([&](XMLDocument& doc, XMLElement* root) {
					auto* el = doc.NewElement("movedAlbum");
					el->SetAttribute("id",          std::to_string(aid).c_str());
					el->SetAttribute("parent",      std::to_string(pid).c_str());
					el->SetAttribute("album",       new_album.c_str());
					el->SetAttribute("artist",      new_folder.c_str());
					el->SetAttribute("tagFailures", 0);
					root->InsertEndChild(el);
					}),
				use_json ? "application/json" : "application/xml");
			return;
			}

		// ---- The move --------------------------------------------------
		fs::path abs_src    = store_.abs_path(old_rel);
		fs::path abs_target = store_.abs_path(new_rel);
		if (abs_src.empty() || abs_target.empty()) {
			err(0, "Could not resolve the library path."); return;
			}
		// Belt and braces over sanitise_component's traversal guard.
		// path_is_within_root uses weakly_canonical, so it answers for a target
		// that does not exist yet.
		if (!store_.path_is_within_root(abs_target)) {
			std::cout << stamp() << "moveAlbum: refusing target outside every root: "
			          << abs_target << std::endl;
			err(0, "Refusing to write outside the configured roots.");
			return;
			}
		if (fs::exists(abs_target)) {
			// Not "for that artist": the destination may be a category.
			err(0, "Something with that name is already in that folder.");
			return;
			}

		std::error_code ec;
		// Collected before the rename, since they are named after the file
		// where it is now.  A file-album's cover, poster, subtitles, chapter
		// markers and liner notes are separate files beside it, and a move
		// that left them behind would strand every one of them somewhere
		// nothing will ever reconnect them from.
		const std::vector<std::string> sidecars =
			file_album ? store_.sidecars_of(abs_src.string())
			           : std::vector<std::string>{};

		// Creating the destination is what makes "type a name that is not in
		// the list" the way to add an artist or a category, with no separate
		// operation for it.
		fs::create_directories(store_.abs_path(new_parent_dir), ec);

		fs::rename(abs_src, abs_target, ec);
		if (ec) {
			// Cross-device: fall back to recursive copy then remove.
			fs::copy(abs_src, abs_target, fs::copy_options::recursive, ec);
			if (ec) { err(0, ("Failed to move item: " + ec.message()).c_str()); return; }
			fs::remove_all(abs_src, ec);
			}

		std::cout << stamp() << "Move: " << old_rel << " → " << new_rel << std::endl;

		// Carry the DB across with the directory.  Without this the rescan
		// below deletes the old folder and inserts the new one cold, and every
		// star, play count, playlist entry and bookmark — all keyed on the path
		// string — quietly stops matching anything, along with the hand-picked
		// cover and any typed video title, and the derived art caches.
		store_.relocate_prefix(old_rel, new_rel);

		// The sidecars follow, each one its own tiny relocate.  The media
		// file's own move above already carried songs.path, chapters.path and
		// the rest; what these calls are for is the columns that name the
		// *image*: cover_thumbs.source_key, songs.cover_path,
		// albums.cover_path.  A rescan would repair the last two and never the
		// first, so the thumbnails would go on being keyed on a path with no
		// file at it.
		//
		// The suffix is whatever follows the old stem — ".jpg", but equally
		// "-poster.jpg" or ".chapters.txt", which is why it is taken by length
		// rather than by extension().
		if (file_album) {
			const std::string old_stem = fs::path(abs_src).stem().string();
			const fs::path    new_stem =
				abs_target.parent_path() / abs_target.stem();
			for (const auto& s : sidecars) {
				std::string fname = fs::path(s).filename().string();
				if (fname.size() <= old_stem.size()) continue;
				std::string suffix = fname.substr(old_stem.size());
				fs::path    dest   = fs::path(new_stem.string() + suffix);
				std::error_code sc;
				if (fs::exists(dest, sc)) continue;   // never overwrite
				fs::rename(s, dest, sc);
				if (sc) {
					std::cout << stamp() << "Move: could not move sidecar " << s
					          << ": " << sc.message() << std::endl;
					continue;
					}
				store_.relocate_prefix(store_.rel_path(s),
				                        store_.rel_path(dest.string()));
				}
			}

		// ---- Then the tags ---------------------------------------------
		//
		// The opposite order to updateSong, which writes tags first so that a
		// failure cannot be recorded in the DB.  There the DB mirrors the tag;
		// here it mirrors the *directory*, so the move is the authoritative act
		// and a file TagLib will not write is a cosmetic loss rather than a
		// desync.  Failures are counted, not fatal.
		//
		// **The artist tag is only written under an artists root.**  Under a
		// categories root the level is a category — Film, Series — and writing
		// that into an artist tag would stamp "Film" across every file of a
		// promoted documentary.  Neither endpoint this replaces got that right:
		// promote wrote no tags at all, so a promoted film kept its channel
		// name, and rename wrote both unconditionally, which was safe only
		// because it could never reach a categories root.
		// Compared as *names*, so a file-album's extension does not make every
		// rename look like an album change — and never reaches a tag.
		const bool write_album  = (new_album  != old_album_name);
		const bool write_artist = (new_folder != old_folder)
		                          && dest_root_type == "artists";
		int tag_failures = 0;
		auto tag_one = [&](const fs::path& p) {
			try {
				TagLib::FileRef f(p.c_str());
				if (f.isNull() || !f.tag()) return;   // not a taggable file
				if (write_album)
					f.tag()->setAlbum(TagLib::String(new_album, TagLib::String::UTF8));
				// Only overwrite an artist tag that was the old folder's
				// name anyway.  A tag naming somebody else is a real
				// credit — every track of a compilation has one — and this
				// used to flatten the lot to "Various Artists" on a move.
				// Worse, saving bumps the mtime, so the next scan re-read
				// the value it had just destroyed and the library
				// converged on it with nothing left to recover from.
				if (write_artist) {
					std::string cur = f.tag()->artist().to8Bit(true);
					if (cur.empty() || artist_key(cur) == artist_key(old_folder))
						f.tag()->setArtist(
							TagLib::String(new_folder, TagLib::String::UTF8));
					}
				if (!f.save()) {
					++tag_failures;
					std::cout << stamp() << "Move: could not write tags to "
					          << p << std::endl;
					}
				}
			catch (const std::exception& ex) {
				++tag_failures;
				std::cout << stamp() << "Move: exception tagging " << p
				          << ": " << ex.what() << std::endl;
				}
			catch (...) {
				++tag_failures;
				std::cout << stamp() << "Move: unknown exception tagging "
				          << p << std::endl;
				}
			};
		if (write_album || write_artist) {
			// One file or a directory of them.  The walk is not merely
			// unnecessary for a file-album: recursive_directory_iterator on a
			// regular file sets `ec` and iterates zero times, so the rename
			// would report tagFailures 0 having written nothing at all.
			if (file_album)
				tag_one(abs_target);
			else
				for (auto& e : fs::recursive_directory_iterator(abs_target, ec)) {
					if (!e.is_regular_file()) continue;
					tag_one(e.path());
					}
			}

		// An emptied source directory is gone as far as the library is
		// concerned; removing it lets the rescan below prune its folder row.
		// Left in place it is an artist reading "0 albums", which is the shape
		// every stranding here takes.
		//
		// **Only when there was an artist level to empty.**  A level-1
		// file-album — a loose film in a root used as one flat library — leaves
		// parts.size() == 2, and its "parent" is the root itself, which
		// fs::is_empty would happily report on and fs::remove would then delete
		// out from under the server.  (Before a loose file became its own
		// album, the case was a loose-file *section* moved out of a root; the
		// arithmetic and the hazard are the same.)
		fs::path old_parent_abs = store_.abs_path(old_parent_dir);
		bool old_parent_gone = false;
		if (parts.size() >= 3
		        && fs::is_directory(old_parent_abs, ec)
		        && fs::is_empty(old_parent_abs, ec)) {
			fs::remove(old_parent_abs, ec);
			old_parent_gone = true;
			}
		// And the batch directory above it, as deleteUpload does: the <uuid>
		// level never gets a folder row of its own — scan_artist_dir parents an
		// artist directory straight to the root — so there is nothing to
		// rescan for it, only a directory to not leave behind.
		if (old_parent_gone && !uploads_root_name_.empty()
		        && parts[0] == uploads_root_name_ && parts.size() >= 5) {
			fs::path batch_abs = old_parent_abs.parent_path();
			if (fs::is_directory(batch_abs, ec) && fs::is_empty(batch_abs, ec))
				fs::remove(batch_abs, ec);
			}

		// Synchronously, so the response is never ahead of the database. After
		// the relocate above this is a consistency pass rather than a rebuild:
		// the rows already carry the new paths, so the walk re-stamps them and
		// prunes nothing. It is also what re-establishes the album's artist
		// link, which relocate_prefix dropped — so a contended database here
		// would leave the album with no artist until the next scan, and that is
		// worth a log line rather than a 500 on a move that has already
		// happened.
		//
		// DESTINATION FIRST, and it must be two calls rather than one set:
		// scan_dirs takes a std::set, so a single call would scan them in
		// whatever order the names happen to sort in. The relocate moved the
		// album folder's *path* out from under the old parent but left its
		// parent_id pointing at the old parent's row, and folders.parent_id has
		// no ON DELETE CASCADE. Tearing down the source first therefore hits
		// FOREIGN KEY constraint failed, which rolls the whole prune back and
		// leaves the emptied artist behind — with no error anywhere, because
		// the prune catches. Scanning the destination first re-points
		// parent_id, and the source then deletes cleanly.
		//
		// **A level-1 file-album is rescanned by its own path, not its
		// parent's.** Its parent is the bare root name, which scan_dirs()
		// deliberately escalates to a full scan() — minutes of work to settle
		// a rename. Handing it the file path instead lands in the file-album
		// branch of scan_dirs(), which calls scan_root_files() for that root
		// and nothing else. Both ends are checked separately because a move
		// can be level-1 at one end only.
		//
		// The ordering rule is unaffected: for such a move both ends are in
		// the same root and neither re-parents anything, so there is no
		// dangling parent_id for the source pass to trip over.
		auto components = [](const std::string& rel) {
			int n = 0;
			for (const auto& c : fs::path(rel)) { (void)c; ++n; }
			return n;
			};
		const std::string scan_dest = (components(new_rel) < 3) ? new_rel
		                                                        : new_parent_dir;
		const std::string scan_src  = (parts.size() < 3) ? old_rel
		                                                 : old_parent_dir;
		try {
			store_.scan_dirs({scan_dest});
			if (scan_src != scan_dest)
				store_.scan_dirs({scan_src});
			}
		catch (const std::exception& e) {
			std::cout << stamp() << "Move: rescan failed: " << e.what() << std::endl;
			}
		catch (...) {
			std::cout << stamp() << "Move: rescan failed: unknown exception"
			          << std::endl;
			}

		// The client needs the new ids to navigate to what it just moved. They
		// are usually unchanged — that is the point of relocating rather than
		// letting the rescan rebuild — but a move into a directory that did not
		// exist yet mints one.
		int new_album_id  = 0;
		int new_parent_id = 0;
		if (auto f = store_.folder_id_by_path(new_rel))        new_album_id  = *f;
		if (auto f = store_.folder_id_by_path(new_parent_dir)) new_parent_id = *f;

		std::string body = use_json
			? subsonic_ok_json([&](nlohmann::json& r) {
				r["movedAlbum"]["id"]          = std::to_string(new_album_id);
				r["movedAlbum"]["parent"]      = std::to_string(new_parent_id);
				r["movedAlbum"]["album"]       = new_album;
				r["movedAlbum"]["artist"]      = new_folder;
				r["movedAlbum"]["tagFailures"] = tag_failures;
				})
			: subsonic_ok([&](XMLDocument& doc, XMLElement* root) {
				auto* el = doc.NewElement("movedAlbum");
				el->SetAttribute("id",          std::to_string(new_album_id).c_str());
				el->SetAttribute("parent",      std::to_string(new_parent_id).c_str());
				el->SetAttribute("album",       new_album.c_str());
				el->SetAttribute("artist",      new_folder.c_str());
				el->SetAttribute("tagFailures", tag_failures);
				root->InsertEndChild(el);
				});
		res.set_content(body, use_json ? "application/json" : "application/xml");
		});

	// deleteUpload — remove one of the caller's own uploaded albums.
	//
	// Param: id (album folder_id). The way out of a fetch that produced the
	// wrong thing; before this the only route out of the uploads area was
	// promoting into the shared library, or shell access to the server.
	//
	// **The path check below is the security boundary, not a validation
	// nicety.** Without it this is "delete the folder with this id", which is
	// "delete any folder on the server", reachable by any account with upload
	// rights guessing integers. Two things have to hold whoever is asking: the
	// path is exactly five components, and the first is the uploads root.
	//
	// The third — that the second component is the caller — holds for everyone
	// but an admin. An admin may delete anybody's upload, because an admin is
	// who approves them: `personal=*` lets them see everyone's, and being able
	// to see junk without being able to clear it would leave them asking its
	// owner to do it.
	//
	// That widening is also what keeps the clients simple. Both gate the action
	// on "did I reach this through Uploads" and nothing else, which stays
	// correct precisely because a non-admin can only ever reach their own.
	server_.Get("/rest/deleteUpload.view", [this](const httplib::Request& req,
	                                               httplib::Response& res) {
		if (!check_auth(req, res, store_)) return;
		bool use_json = (fmt_of(req) == "json");

		auto err = [&](int code, const char* msg) {
			res.set_content(use_json ? subsonic_error_json(code, msg)
			                         : subsonic_error(code, msg),
			                use_json ? "application/json" : "application/xml");
			};

		if (!check_upload_perm(req, res, store_, use_json)) return;
		const std::string uname = req.get_param_value("u");
		auto ui = store_.get_user(uname);
		const bool is_admin = ui && ui->is_admin;

		auto id_it = req.params.find("id");
		if (id_it == req.params.end()) { err(10, "Missing parameter: id."); return; }

		auto rel_opt = store_.album_folder_path_by_id(to_int(id_it->second, 0));
		if (!rel_opt) { err(70, "Item not found."); return; }
		const std::string& item_rel = *rel_opt;

		namespace fs = std::filesystem;
		fs::path rel_p(item_rel);
		std::vector<std::string> parts;
		// const auto&, not auto&: path::iterator's reference type is
		// implementation-defined. libstdc++ hands out a const path&, but libc++
		// returns a path by value, and a non-const lvalue reference cannot bind
		// to that temporary.
		for (const auto& c : rel_p) parts.push_back(c.string());
		if (parts.size() != 5 || uploads_root_name_.empty()
		        || parts[0] != uploads_root_name_
		        || (parts[1] != uname && !is_admin)) {
			// One message for "not an upload" and "not yours", deliberately: the
			// difference is only useful to somebody probing ids.
			err(0, "Item is not in your uploads.");
			return;
			}
		// The *owner's* name, which is the caller's own except for an admin
		// clearing somebody else's — and it is the owner's directories that get
		// tidied up below, not the caller's.
		const std::string& owner       = parts[1];
		const std::string& batch_uuid  = parts[2];
		const std::string& artist_name = parts[3];

		fs::path abs = store_.abs_path(item_rel);
		// Belt and braces over the component check, the pairing moveAlbum
		// keeps: that establishes the shape, this establishes that the shape
		// resolves to somewhere inside a root. A symlink escaping to /etc
		// prefixes none of them.
		if (!store_.path_is_within_root(abs)) {
			err(0, "Item is not in your uploads."); return;
			}

		std::error_code ec;
		// A file-album's companions go with it.  remove_all() on a directory
		// takes everything inside; on a single media file it takes the file
		// and leaves the cover, poster, subtitles, chapter markers and liner
		// notes sitting in the section, where nothing afterwards will ever
		// remove them and nothing will ever use them again.  Collected before
		// the delete, since they are found by the file's own name.
		std::vector<std::string> sidecars;
		if (fs::is_regular_file(abs, ec)) sidecars = store_.sidecars_of(abs.string());

		fs::remove_all(abs, ec);
		if (ec) { err(0, ("Failed to delete item: " + ec.message()).c_str()); return; }
		for (const auto& s : sidecars) {
			std::error_code sc;
			fs::remove(s, sc);
			store_.forget_prefix(store_.rel_path(s));
			}

		// Before the rescan, and it is the half the rescan will not do: the
		// scanner prunes the music DB and its derived caches but has never
		// touched the client schema, so stars, play counts, playlist entries,
		// the queue and bookmarks would outlive the files. That matters more
		// than it used to — a later batch folded into an earlier one can put a
		// new file at exactly this path, and a surviving star would attach
		// itself to it.
		store_.forget_prefix(item_rel);

		// An emptied artist directory left in place is a folder row reading
		// "0 albums" in the listing, which is the shape every stranding here
		// takes. Removing it makes scan_artist_dir treat it as gone and prune
		// it — the same two steps moveAlbum takes for the same reason.
		std::string artist_rel =
			uploads_root_name_ + "/" + owner + "/" + batch_uuid + "/" + artist_name;
		fs::path artist_abs = store_.abs_path(artist_rel);
		if (fs::is_directory(artist_abs, ec) && fs::is_empty(artist_abs, ec))
			fs::remove(artist_abs, ec);

		// Synchronously, so the response is never ahead of the database. One
		// call covers both outcomes: the artist directory either still stands
		// with fewer albums, or has gone and is pruned.
		store_.scan_dirs({artist_rel});

		// And the batch after it. Nothing to rescan for this one — the <uuid>
		// level never gets a folder row, because scan_artist_dir parents an
		// artist directory straight to the root.
		fs::path batch_abs = artist_abs.parent_path();
		if (fs::is_directory(batch_abs, ec) && fs::is_empty(batch_abs, ec))
			fs::remove(batch_abs, ec);

		// Says who asked as well as what went, because those differ when an
		// admin clears somebody else's and the log is the only record of it.
		std::cout << stamp() << "Delete upload: " << uname << " removed "
		          << item_rel << std::endl;

		res.set_content(use_json ? subsonic_ok_json() : subsonic_ok(),
		                use_json ? "application/json" : "application/xml");
		});

	}
