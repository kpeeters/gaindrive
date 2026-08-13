#include "mediastore.hh"
#include "stamp.hh"
#include "md5.hh"

#include <iostream>
#include <algorithm>
#include <chrono>
#include <ctime>
#include <regex>
#include <set>
#include <unordered_map>

#include <taglib/fileref.h>
#include <tfilestream.h>
#include <taglib/tag.h>
#include <taglib/audioproperties.h>
#include <tpropertymap.h>

#include <nlohmann/json.hpp>
#include <reproc++/reproc.hpp>
#include <reproc++/drain.hpp>

#include "dvd.hh"
#include "tmdb.hh"
#include "videoname.hh"

namespace fs = std::filesystem;

// How long a contended write waits before SQLite reports SQLITE_BUSY.
// Generous enough to absorb the brief overlaps that happen in practice, short
// enough that a writer which is genuinely stuck gets reported rather than
// hanging the scan indefinitely.
static constexpr int DB_BUSY_TIMEOUT_MS = 10000;

static const std::set<std::string> AUDIO_EXTENSIONS = {
	".flac", ".mp3", ".ogg", ".oga", ".m4a", ".aac", ".wav", ".opus", ".wma"
	};

// Video sources.  Kept in sync with VIDEO_TARGETS in codecs.hh, which maps the
// same extensions to MIME types; this set is the scanner's admission test and
// that table is the serving side.
static const std::set<std::string> VIDEO_EXTENSIONS = {
	".mkv", ".mp4", ".m4v", ".avi", ".mpg", ".mpeg", ".mov", ".webm", ".wmv",
	".vob"
	};

// Candidate cover art filenames in priority order.  Add more here as needed.
static const std::vector<std::string> COVER_FILENAMES = {
	"cover.jpg", "cover.jpeg", "folder.jpg", "folder.jpeg",
	"front.jpg", "front.jpeg", "cover.png"
	};

static bool iends_with(const std::string& s, const std::string& suffix)
	{
	if (suffix.size() > s.size()) return false;
	std::string lo = s;
	std::transform(lo.begin(), lo.end(), lo.begin(), ::tolower);
	return lo.compare(lo.size() - suffix.size(), suffix.size(), suffix) == 0;
	}

// recurse=false is for a folder that is an album *and* a parent of albums —
// a section holding loose files next to its subfolders.  Pass 5 would then
// hand the section one of its own albums' covers.
static std::string find_cover(const fs::path& dir, bool recurse = true)
	{
	// Pass 1: exact well-known names.
	for (auto& name : COVER_FILENAMES) {
		auto p = dir / name;
		if (fs::exists(p)) return p.string();
		}
	// Pass 2: *front.jpg/jpeg  Pass 3: *.jpg/jpeg  Pass 4: any image (single scan for all).
	std::string jpg_fallback;
	std::string any_img_fallback;
	for (auto& entry : fs::directory_iterator(dir)) {
		if (!entry.is_regular_file()) continue;
		std::string fname = entry.path().filename().string();
		if (iends_with(fname, "front.jpg") || iends_with(fname, "front.jpeg"))
			return entry.path().string();
		if (jpg_fallback.empty() && (iends_with(fname, ".jpg") || iends_with(fname, ".jpeg")))
			jpg_fallback = entry.path().string();
		if (any_img_fallback.empty() && iends_with(fname, ".png"))
			any_img_fallback = entry.path().string();
		}
	if (!jpg_fallback.empty()) return jpg_fallback;
	if (!any_img_fallback.empty()) return any_img_fallback;
	// Pass 5: recurse into subdirectories for any image.
	if (!recurse) return "";
	for (auto& entry : fs::recursive_directory_iterator(dir)) {
		if (!entry.is_regular_file()) continue;
		std::string fname = entry.path().filename().string();
		if (iends_with(fname, ".jpg") || iends_with(fname, ".jpeg") || iends_with(fname, ".png"))
			return entry.path().string();
		}
	return "";
	}

// A poster belonging to one file rather than to a folder: "film.mp4" is
// matched by "film.jpg" or "film-poster.jpg".  That is what Kodi and Jellyfin
// read beside a flat movie file, and it is the only way a loose file can carry
// a cover of its own — a folder cover belongs to the whole section it sits in.
static std::string find_song_cover(const fs::path& file)
	{
	static const std::vector<std::string> SUFFIXES = {
		".jpg", ".jpeg", ".png",
		"-poster.jpg", "-poster.jpeg", "-poster.png"
		};
	std::string stem = (file.parent_path() / file.stem()).string();
	for (auto& suffix : SUFFIXES) {
		fs::path p = stem + suffix;
		if (fs::exists(p)) return p.string();
		}
	return "";
	}

// Cover art for a song row: its own sidecar image when it has one, otherwise
// the album folder's cover.  al.folder_id rather than s.folder_id because a
// song on a multi-disc album lives in a disc subfolder, which has no albums
// row to resolve a cover on.  See MediaStore::SONG_COVER_ID_BASE for why a
// song's own cover is an offset id.
static const std::string SONG_COVER_ART_SQL =
	"       CASE WHEN s.cover_path IS NOT NULL AND s.cover_path != ''"
	"            THEN " + std::to_string(MediaStore::SONG_COVER_ID_BASE) + " + s.id"
	"            WHEN al.cover_path IS NOT NULL AND al.cover_path != ''"
	"            THEN COALESCE(al.folder_id, s.folder_id)"
	"            ELSE -1 END AS cover_art_id,";

// The artist folder of an album whose folder is aliased `f`.  Normally the
// folder above it, but a folder that directly contains media is itself an
// album, and when that folder is a level-1 one (its parent is a root) or a
// root, it is its own artist — the section holding loose files is both.
// Reporting the parent there would name the *root*, and a client that anchors
// its album list on this field lands on the list of sections.
static const std::string ALBUM_ARTIST_ID_SQL =
	"       CASE WHEN f.parent_id IS NULL"
	"              OR (SELECT p.parent_id FROM folders p WHERE p.id = f.parent_id)"
	"                 IS NULL"
	"            THEN f.id ELSE f.parent_id END,";

// Returns sorted list of image paths in dir, excluding cover_path.  Recursive
// unless the caller says otherwise — see get_extra_image_paths(), where a
// folder with subfolders is a section whose albums' images are not its own.
static std::vector<std::string> find_extra_images(const fs::path& dir,
                                                   const std::string& cover_path,
                                                   bool recurse = true)
	{
	static const std::set<std::string> IMG_EXT = {".jpg", ".jpeg", ".png"};
	std::vector<std::string> result;
	auto consider = [&](const fs::directory_entry& entry) {
		if (!entry.is_regular_file()) return;
		std::string ext = entry.path().extension().string();
		std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
		if (!IMG_EXT.count(ext)) return;
		if (entry.path().string() == cover_path) return;
		result.push_back(entry.path().string());
		};
	try {
		if (recurse)
			for (auto& entry : fs::recursive_directory_iterator(dir)) consider(entry);
		else
			for (auto& entry : fs::directory_iterator(dir))           consider(entry);
		}
	catch (...) {}
	std::sort(result.begin(), result.end());
	return result;
	}

static std::string lower_ext(const fs::path& p)
	{
	std::string ext = p.extension().string();
	std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
	return ext;
	}

static bool is_video_file(const fs::path& p)
	{
	return VIDEO_EXTENSIONS.count(lower_ext(p)) > 0;
	}

// Admission test for the scanner.  Audio and video share the songs table and
// therefore share this gate; is_video_file() decides which metadata reader
// runs later.
static bool is_media_file(const fs::path& p)
	{
	std::string ext = lower_ext(p);
	return AUDIO_EXTENSIONS.count(ext) > 0 || VIDEO_EXTENSIONS.count(ext) > 0;
	}

// ffprobe reports every numeric field as a JSON *string*, but not every field
// is present in every container.  Returns 0 for missing, null or unparseable.
static double probe_num(const nlohmann::json& j, const char* key)
	{
	auto it = j.find(key);
	if (it == j.end() || it->is_null()) return 0;
	if (it->is_number()) return it->get<double>();
	if (it->is_string()) {
		try { return std::stod(it->get<std::string>()); }
		catch (...) { return 0; }
		}
	return 0;
	}

// Everything the streamer and the video endpoints need to know about a video
// file.  TagLib cannot open these containers at all, so unlike the audio path
// there is no fallback reader: without ffprobe a video row has no duration, no
// bitrate and no dimensions.
struct VideoProbe
	{
	double      duration = 0;
	int         bitrate  = 0;   // kbps, the same unit the audio path stores
	int         width    = 0;
	int         height   = 0;
	std::string video_codec;
	std::string audio_codec;
	};

// A probe failure is deliberately not fatal.  The caller still creates the
// row: a video that plays but reports duration 0 is a better outcome than a
// file silently missing from the library, and the next scan retries.
static std::optional<VideoProbe> probe_video(const std::string& path)
	{
	std::vector<std::string> args = {
		"ffprobe", "-v", "quiet", "-print_format", "json",
		"-show_format", "-show_streams", path
		};

	reproc::process proc;
	reproc::options opts;
	opts.redirect.err.type = reproc::redirect::type::discard;
	if (proc.start(args, opts)) return std::nullopt;

	std::string          out;
	reproc::sink::string sink(out);
	auto ec            = reproc::drain(proc, sink, reproc::sink::null);
	auto [status, wec] = proc.wait(reproc::infinite);
	if (ec || wec || status != 0) return std::nullopt;

	VideoProbe vp;
	try {
		auto j = nlohmann::json::parse(out);

		if (auto f = j.find("format"); f != j.end()) {
			vp.duration = probe_num(*f, "duration");
			vp.bitrate  = static_cast<int>(probe_num(*f, "bit_rate") / 1000.0);
			}

		auto streams = j.find("streams");
		if (streams != j.end() && streams->is_array())
			for (const auto& s : *streams) {
				auto type = s.value("codec_type", std::string());
				// An embedded cover image is carried as a video stream with
				// attached_pic set.  Taking it as *the* video stream would
				// describe an m4a-style cover as a 600x600 mjpeg "movie".
				int attached = 0;
				if (auto d = s.find("disposition"); d != s.end())
					attached = d->value("attached_pic", 0);
				if (type == "video" && vp.video_codec.empty() && attached == 0) {
					vp.video_codec = s.value("codec_name", std::string());
					vp.width       = static_cast<int>(probe_num(s, "width"));
					vp.height      = static_cast<int>(probe_num(s, "height"));
					}
				else if (type == "audio" && vp.audio_codec.empty())
					vp.audio_codec = s.value("codec_name", std::string());
				}
		}
	catch (const std::exception&) { return std::nullopt; }

	// Containers that omit format.bit_rate (some MKVs) still have a size and a
	// duration, and the throttle needs a non-zero figure to pace with.
	if (vp.bitrate <= 0 && vp.duration > 0) {
		std::error_code fec;
		auto size = fs::file_size(path, fec);
		if (!fec && size > 0)
			vp.bitrate = static_cast<int>(
				static_cast<double>(size) * 8.0 / vp.duration / 1000.0);
		}

	return vp;
	}

// Returns file modification time as Unix seconds.
static int64_t mtime_of(const fs::path& p)
	{
	auto lwt  = fs::last_write_time(p);
	auto sys  = std::chrono::file_clock::to_sys(lwt);
	return std::chrono::duration_cast<std::chrono::seconds>(
		sys.time_since_epoch()).count();
	}

// Helpers for the music-root-relative path convention. All paths persisted in
// the music DB (folders.path, songs.path, albums.cover_path) and the client DB
// (client.*) are stored as "<root name>/<path within that root>"; strip_root()
// converts an absolute path to that form on write, and join_root() composes the
// absolute form when a filesystem call needs it.

const MediaStore::RootRec* MediaStore::root_for_abs(const std::string& abs) const
	{
	for (const auto& r : roots_) {
		if (abs == r.cfg.path) return &r;
		if (abs.size() > r.path_slash.size()
		    && abs.compare(0, r.path_slash.size(), r.path_slash) == 0)
			return &r;
		}
	return nullptr;
	}

const MediaStore::RootRec* MediaStore::root_for_rel(const std::string& rel) const
	{
	auto slash = rel.find('/');
	std::string name = (slash == std::string::npos) ? rel : rel.substr(0, slash);
	for (const auto& r : roots_)
		if (r.cfg.name == name) return &r;
	return nullptr;
	}

// Absolute path -> "<root name>/<path within that root>".
std::string MediaStore::strip_root(const std::string& abs) const
	{
	const RootRec* r = root_for_abs(abs);
	if (!r) return abs;   // outside every root — shouldn't happen; pass through
	if (abs == r->cfg.path) return r->cfg.name;
	return r->cfg.name + "/" + abs.substr(r->path_slash.size());
	}

// "<root name>/<rest>" -> absolute path.  An unknown root name yields an empty
// string rather than a path built from a guess: every caller feeds the result
// to the filesystem, and a plausible-but-wrong path is worse than a failure.
std::string MediaStore::join_root(const std::string& rel) const
	{
	if (rel.empty()) return rel;
	const RootRec* r = root_for_rel(rel);
	if (!r) return {};
	auto slash = rel.find('/');
	if (slash == std::string::npos) return r->cfg.path;
	return r->path_slash + rel.substr(slash + 1);
	}

// Defence-in-depth: returns true iff the canonicalised candidate sits within
// the (already-canonicalised) base. Used to refuse any filesystem operation on
// a path that — after symlink resolution — escapes every root. The bases are
// canonicalised once at MediaStore ctor; the candidate is canonicalised every
// call.
static bool is_within(const fs::path& candidate, const fs::path& canonical_base)
	{
	std::error_code ec;
	auto c = fs::weakly_canonical(candidate, ec);
	if (ec) return false;
	auto [it_b, it_c] = std::mismatch(canonical_base.begin(), canonical_base.end(),
	                                   c.begin(), c.end());
	return it_b == canonical_base.end();
	}

// ---- MediaStore -------------------------------------------------------

// Insert suffix before ".db" extension, or append if no extension.
static std::string derive_path(const std::string& base, const std::string& suffix)
	{
	auto dot = base.rfind(".db");
	if (dot != std::string::npos && dot == base.size() - 3)
		return base.substr(0, dot) + suffix + ".db";
	return base + suffix;
	}

MediaStore::MediaStore(const std::string& db_path, const std::vector<Root>& roots,
                       const std::string& user_db_path, int video_art_px,
                       bool video_art_frames)
	// The third argument is the busy timeout, and it defaults to 0 — meaning
	// SQLite gives up on a contended write *immediately* and SQLiteCpp turns
	// that into a throw.  Any external writer (a second gaindrive, or sqlite3
	// running BEGIN IMMEDIATE) would therefore kill a scan in progress, even
	// though the contention is almost always momentary.  Waiting is the whole
	// fix for the common case; the exception handling around the scan covers
	// a writer that genuinely sits on the lock.
	//
	// Set here rather than by PRAGMA because the pragmas below — including
	// journal_mode=WAL — can themselves hit a locked database.  The timeout is
	// a property of the connection, so it also covers the attached client
	// schema.  db_mutex_ is no help: it serialises our own threads, and this
	// contention is between processes.
	: db_music_(derive_path(db_path, "-music"),
	            SQLite::OPEN_READWRITE | SQLite::OPEN_CREATE,
	            DB_BUSY_TIMEOUT_MS)
	{
	if (video_art_px > 0) video_art_.emplace(video_art_px, video_art_frames);

	// Normalise each root: strip trailing '/' so path never has one, derive
	// the always-has-one form used to compose and strip absolute paths, and
	// canonicalise once. path_is_within_root() compares against the canonical
	// forms, so a symlink inside a root that escapes it is caught at every
	// file open — while a root that is *itself* a symlink still works, since
	// its own canonical form is one of the bases.
	for (const auto& r : roots) {
		RootRec rec;
		rec.cfg = r;
		while (rec.cfg.path.size() > 1 && rec.cfg.path.back() == '/')
			rec.cfg.path.pop_back();
		rec.path_slash = rec.cfg.path + "/";
		std::error_code ec;
		rec.canonical = fs::weakly_canonical(fs::path(rec.cfg.path), ec);
		if (ec) rec.canonical = fs::path(rec.cfg.path);
		roots_.push_back(std::move(rec));
		}
	for (const auto& r : roots_)
		roots_public_.push_back(r.cfg);
	for (const auto& r : roots_)
		if (r.cfg.type == "uploads") {
			uploads_prefix_ = r.cfg.name + "/";
			uploads_like_   = r.cfg.name + "/%";
			break;
			}

	// User/state DB: explicit override if given, else derived from db_path.
	std::string client_path = user_db_path.empty()
	                          ? derive_path(db_path, "-client") : user_db_path;
	db_music_.exec("PRAGMA journal_mode=WAL");
	db_music_.exec("PRAGMA foreign_keys=ON");
	db_music_.exec("ATTACH DATABASE '" + client_path + "' AS client");
	db_music_.exec("PRAGMA client.journal_mode=WAL");
	db_music_.exec("PRAGMA client.foreign_keys=ON");
	create_schema();
	purge_disabled_video_art();
	sync_roots();
	}

// Frames stored by an earlier run, when the tier producing them is now off.
//
// Leaving them would make "off" mean only "stop making new ones", which is not
// what anybody asking for it wants: the useless frames would go on being the
// cover art for the whole collection. They are cheap to get back — one rescan
// with the tier enabled — which is what makes deleting them the right default
// rather than a destructive one.
void MediaStore::purge_disabled_video_art()
	{
	std::lock_guard<std::mutex> lock(db_mutex_);
	SQLite::Transaction txn(db_music_);

	if (!video_art_ || !video_art_->frames_allowed()) {
		db_music_.exec("DELETE FROM video_art WHERE source = 'frame'");
		if (int n = db_music_.getChanges(); n > 0)
			std::cout << stamp() << "video art: dropped " << n
			          << " frame grabs (the frame tier is off)" << std::endl;
		}

	// Whatever the reason a row went away, the cover_path pointing at it has
	// to go too. A cover_path that is also a songs.path is by construction the
	// video-art convention — a real image is never a song — so this repairs
	// exactly the dangling pointers and nothing else. Without it the art is
	// gone but every affected album still claims to have some, and getCoverArt
	// answers 404 for a cover the client was told existed.
	db_music_.exec(
		"UPDATE albums SET cover_path = ''"
		" WHERE cover_path IN (SELECT path FROM songs)"
		"   AND cover_path NOT IN (SELECT path FROM video_art)");
	db_music_.exec(
		"UPDATE songs SET cover_path = ''"
		" WHERE cover_path IN (SELECT path FROM songs)"
		"   AND cover_path NOT IN (SELECT path FROM video_art)");

	txn.commit();
	}

void MediaStore::create_schema()
	{
	SQLite::Transaction txn(db_music_);

	// Music library tables (gaindrive-music.db, main schema).
	db_music_.exec(R"(
		CREATE TABLE IF NOT EXISTS folders (
			id           INTEGER PRIMARY KEY,
			parent_id    INTEGER REFERENCES folders(id),
			path         TEXT NOT NULL UNIQUE,    -- "<root>/<rest>"; a root row stores just "<root>"
			name         TEXT NOT NULL,
			-- Set only on root rows: "artists" or "categories". Refreshed from
			-- the configuration on every start, so this is a cache of config
			-- rather than a source of truth.
			content_type TEXT,
			last_scanned DATETIME
		);
		CREATE INDEX IF NOT EXISTS idx_folders_parent ON folders(parent_id);

		CREATE TABLE IF NOT EXISTS artists (
			id             INTEGER PRIMARY KEY,
			name           TEXT NOT NULL,
			sort_name      TEXT,
			musicbrainz_id TEXT,
			image_path     TEXT,
			biography      TEXT,
			UNIQUE(name)
		);

		CREATE TABLE IF NOT EXISTS albums (
			id             INTEGER PRIMARY KEY,
			folder_id      INTEGER NOT NULL REFERENCES folders(id) UNIQUE,
			title          TEXT NOT NULL,
			sort_title     TEXT,
			year           INTEGER,
			genre          TEXT,
			disc_count     INTEGER DEFAULT 1,
			duration       REAL DEFAULT 0,
			song_count     INTEGER DEFAULT 0,
			cover_path     TEXT,                  -- "<root>/<rest>"
			musicbrainz_id TEXT,
			created        DATETIME DEFAULT CURRENT_TIMESTAMP,
			last_scanned   DATETIME
		);
		CREATE INDEX IF NOT EXISTS idx_albums_folder ON albums(folder_id);

		CREATE TABLE IF NOT EXISTS album_artists (
			album_id      INTEGER NOT NULL REFERENCES albums(id)  ON DELETE CASCADE,
			artist_id     INTEGER NOT NULL REFERENCES artists(id) ON DELETE CASCADE,
			role          TEXT NOT NULL DEFAULT 'albumartist',
			display_order INTEGER DEFAULT 0,
			PRIMARY KEY (album_id, artist_id, role)
		);
		CREATE INDEX IF NOT EXISTS idx_album_artists_artist ON album_artists(artist_id);
		CREATE INDEX IF NOT EXISTS idx_album_artists_role   ON album_artists(role);

		CREATE TABLE IF NOT EXISTS songs (
			id                  INTEGER PRIMARY KEY,
			album_id            INTEGER NOT NULL REFERENCES albums(id) ON DELETE CASCADE,
			folder_id           INTEGER NOT NULL REFERENCES folders(id),
			path                TEXT NOT NULL UNIQUE, -- "<root>/<rest>"
			filename            TEXT NOT NULL,
			title               TEXT NOT NULL,
			sort_title          TEXT,
			track_number        INTEGER,
			disc_number         INTEGER DEFAULT 1,
			year                INTEGER,
			genre               TEXT,
			duration            REAL NOT NULL DEFAULT 0,
			bitrate             INTEGER,
			sample_rate         INTEGER,
			channels            INTEGER,
			codec               TEXT,
			file_size           INTEGER,
			-- Video rows live in this table too: every piece of client state
			-- (stars, play counts, playlists, queue, bookmarks) joins on
			-- songs.path, so a separate videos table would mean duplicating
			-- all of it.  Subsonic makes the same choice.
			is_video            INTEGER DEFAULT 0,
			width               INTEGER DEFAULT 0,
			height              INTEGER DEFAULT 0,
			video_codec         TEXT,
			audio_codec         TEXT,
			-- Sidecar image beside this file ("<root>/<rest>"), for a loose
			-- file whose folder cover belongs to a whole section rather than
			-- to it. Empty for everything that inherits its album's cover.
			cover_path          TEXT,
			has_embedded_cover  INTEGER DEFAULT 0,
			musicbrainz_id      TEXT,
			file_modified       INTEGER,
			created             DATETIME DEFAULT CURRENT_TIMESTAMP,
			last_scanned        DATETIME
		);
		CREATE INDEX IF NOT EXISTS idx_songs_album  ON songs(album_id);
		CREATE INDEX IF NOT EXISTS idx_songs_folder ON songs(folder_id);
		CREATE INDEX IF NOT EXISTS idx_songs_genre  ON songs(genre);

		CREATE TABLE IF NOT EXISTS song_artists (
			song_id       INTEGER NOT NULL REFERENCES songs(id)   ON DELETE CASCADE,
			artist_id     INTEGER NOT NULL REFERENCES artists(id) ON DELETE CASCADE,
			role          TEXT NOT NULL DEFAULT 'artist',
			display_order INTEGER DEFAULT 0,
			PRIMARY KEY (song_id, artist_id, role)
		);
		CREATE INDEX IF NOT EXISTS idx_song_artists_artist ON song_artists(artist_id);
		CREATE INDEX IF NOT EXISTS idx_song_artists_role   ON song_artists(role);

		CREATE TABLE IF NOT EXISTS artist_info_cache (
			folder_id    INTEGER PRIMARY KEY REFERENCES folders(id),
			mbid         TEXT NOT NULL DEFAULT '',
			last_fm_url  TEXT NOT NULL DEFAULT '',
			biography    TEXT NOT NULL DEFAULT '',
			image_url    TEXT NOT NULL DEFAULT '',
			wiki_url     TEXT NOT NULL DEFAULT '',
			allmusic_url TEXT NOT NULL DEFAULT '',
			discogs_url  TEXT NOT NULL DEFAULT '',
			fetched_at   INTEGER NOT NULL DEFAULT (strftime('%s','now'))
		);

		CREATE TABLE IF NOT EXISTS album_info_cache (
			folder_id    INTEGER PRIMARY KEY REFERENCES folders(id),
			mbid         TEXT NOT NULL DEFAULT '',
			notes        TEXT NOT NULL DEFAULT '',
			wiki_url     TEXT NOT NULL DEFAULT '',
			allmusic_url TEXT NOT NULL DEFAULT '',
			fetched_at   INTEGER NOT NULL DEFAULT (strftime('%s','now'))
		);

		-- Cover art manufactured from a video file itself, by VideoArt.  Video
		-- containers carry no tag anything actually writes and these files are
		-- rarely named well, so without this every video shows a placeholder.
		-- It sits here with the other derived caches rather than being written
		-- into the library as a sidecar image: nothing gaindrive derives should
		-- land in the user's collection.
		--
		-- Keyed on the stored path, not on songs.id, for the reason stars and
		-- playlists are: a rowid is not stable across a rescan.  file_modified
		-- is what invalidates the art when the file is replaced or re-tagged.
		CREATE TABLE IF NOT EXISTS video_art (
			path          TEXT PRIMARY KEY,   -- "<root>/<rest>"
			file_modified INTEGER NOT NULL,
			mime          TEXT NOT NULL,
			source        TEXT NOT NULL,      -- embedded | frame | tmdb
			image         BLOB NOT NULL,
			created_at    INTEGER NOT NULL DEFAULT (strftime('%s','now'))
		);

		-- What an online provider was asked about a video, and what it said.
		-- The filename parser (src/videoname.hh) produces the question; TMDB
		-- answers with a poster, a plot and a canonical title.
		--
		-- `path` is the *album folder* for a film in a folder of its own, and
		-- the song for a loose file in a section: a film is a folder, so it is
		-- one question however many parts it was split into.
		--
		-- The row exists as much to record a *failure* as a success. Without
		-- it every scan would re-ask about the same unmatchable file forever,
		-- and this is the table that makes a rescan cost no traffic at all.
		-- `query` is what was asked; when the parser produces a different
		-- question — because the file was renamed — the old answer no longer
		-- applies and the lookup runs again.
		CREATE TABLE IF NOT EXISTS video_meta (
			path       TEXT PRIMARY KEY,   -- "<root>/<rest>"
			query       TEXT NOT NULL,     -- "title|year" as asked
			media_type  TEXT NOT NULL,     -- movie | tv
			tmdb_id     INTEGER,
			title       TEXT,
			year        INTEGER,
			overview    TEXT,
			-- Kept so the poster can be re-fetched without asking TMDB who
			-- this is again: art and identity expire for different reasons.
			poster_path TEXT,
			status     TEXT NOT NULL,      -- matched | unmatched | error
			fetched_at INTEGER NOT NULL DEFAULT (strftime('%s','now'))
		);
	)");

	// Client/user data tables (gaindrive-client.db, attached as "client" schema).
	db_music_.exec(R"(
		CREATE TABLE IF NOT EXISTS client.users (
			id             INTEGER PRIMARY KEY,
			username       TEXT NOT NULL UNIQUE,
			password_enc   TEXT NOT NULL,
			email          TEXT,
			is_admin       INTEGER DEFAULT 0,
			max_bitrate    INTEGER DEFAULT 0,
			created        DATETIME DEFAULT CURRENT_TIMESTAMP,
			last_access    DATETIME,
			upload_allowed INTEGER DEFAULT 0,
			disabled       INTEGER DEFAULT 0,
			cast_allowed   INTEGER DEFAULT 0
		);

		-- All references to music-DB rows are by filesystem path, RELATIVE
		-- form "<root name>/<rest>", never by row id. This keeps client data alive
		-- across:
		--   * a music-DB rebuild (rowids reset)
		--   * the rowid churn from upsert_song_with_data's INSERT OR REPLACE
		--   * moving the whole library to a different on-disk location
		-- The music DB stores paths in the same relative form, so cross-DB
		-- JOINs are direct equality, e.g.
		--   JOIN songs s ON s.path = st.song_path

		CREATE TABLE IF NOT EXISTS client.stars (
			user_id              INTEGER NOT NULL REFERENCES users(id) ON DELETE CASCADE,
			song_path            TEXT,
			album_folder_path    TEXT,
			artist_folder_path   TEXT,
			created              DATETIME DEFAULT CURRENT_TIMESTAMP,
			UNIQUE(user_id, song_path, album_folder_path, artist_folder_path)
		);

		CREATE TABLE IF NOT EXISTS client.play_counts (
			user_id     INTEGER NOT NULL REFERENCES users(id) ON DELETE CASCADE,
			song_path   TEXT NOT NULL,
			count       INTEGER DEFAULT 0,
			last_played DATETIME,
			PRIMARY KEY (user_id, song_path)
		);

		CREATE TABLE IF NOT EXISTS client.playlists (
			id        INTEGER PRIMARY KEY,
			user_id   INTEGER NOT NULL REFERENCES users(id) ON DELETE CASCADE,
			name      TEXT NOT NULL,
			comment   TEXT,
			is_public INTEGER DEFAULT 0,
			created   DATETIME DEFAULT CURRENT_TIMESTAMP,
			updated   DATETIME DEFAULT CURRENT_TIMESTAMP
		);

		CREATE TABLE IF NOT EXISTS client.playlist_songs (
			playlist_id INTEGER NOT NULL REFERENCES playlists(id) ON DELETE CASCADE,
			song_path   TEXT NOT NULL,
			position    INTEGER NOT NULL,
			PRIMARY KEY (playlist_id, position)
		);

		CREATE TABLE IF NOT EXISTS client.play_queue (
			user_id    INTEGER NOT NULL REFERENCES users(id) ON DELETE CASCADE,
			song_path  TEXT NOT NULL,
			position   INTEGER NOT NULL,
			is_current INTEGER DEFAULT 0,
			offset_ms  INTEGER DEFAULT 0,
			client     TEXT,
			updated    DATETIME DEFAULT CURRENT_TIMESTAMP,
			PRIMARY KEY (user_id, position)
		);

		CREATE TABLE IF NOT EXISTS client.now_playing (
			user_id   INTEGER NOT NULL REFERENCES users(id) ON DELETE CASCADE,
			song_path TEXT NOT NULL,
			client    TEXT,
			started   DATETIME DEFAULT CURRENT_TIMESTAMP,
			PRIMARY KEY (user_id)
		);

		CREATE TABLE IF NOT EXISTS client.bookmarks (
			user_id   INTEGER NOT NULL REFERENCES users(id) ON DELETE CASCADE,
			song_path TEXT NOT NULL,
			position  INTEGER NOT NULL DEFAULT 0,
			comment   TEXT,
			created   DATETIME DEFAULT CURRENT_TIMESTAMP,
			changed   DATETIME DEFAULT CURRENT_TIMESTAMP,
			PRIMARY KEY (user_id, song_path)
		);

		CREATE TABLE IF NOT EXISTS client.settings (
			key   TEXT PRIMARY KEY,
			value TEXT NOT NULL DEFAULT ''
		);
	)");

	txn.commit();

	// Migrations for existing databases: add columns if they don't exist yet.
	try { db_music_.exec("ALTER TABLE artist_info_cache ADD COLUMN biography TEXT NOT NULL DEFAULT ''"); }
	catch (const SQLite::Exception&) {}
	try { db_music_.exec("ALTER TABLE artist_info_cache ADD COLUMN image_url TEXT NOT NULL DEFAULT ''"); }
	catch (const SQLite::Exception&) {}
	try { db_music_.exec("ALTER TABLE artist_info_cache ADD COLUMN wiki_url TEXT NOT NULL DEFAULT ''"); }
	catch (const SQLite::Exception&) {}
	try { db_music_.exec("ALTER TABLE artist_info_cache ADD COLUMN allmusic_url TEXT NOT NULL DEFAULT ''"); }
	catch (const SQLite::Exception&) {}
	try { db_music_.exec("ALTER TABLE album_info_cache ADD COLUMN allmusic_url TEXT NOT NULL DEFAULT ''"); }
	catch (const SQLite::Exception&) {}
	try { db_music_.exec("ALTER TABLE artist_info_cache ADD COLUMN discogs_url TEXT NOT NULL DEFAULT ''"); }
	catch (const SQLite::Exception&) {}
	try { db_music_.exec("ALTER TABLE client.users ADD COLUMN upload_allowed INTEGER DEFAULT 0"); }
	catch (const SQLite::Exception&) {}
	try { db_music_.exec("ALTER TABLE client.users ADD COLUMN disabled INTEGER DEFAULT 0"); }
	catch (const SQLite::Exception&) {}
	try { db_music_.exec("ALTER TABLE client.users ADD COLUMN cast_allowed INTEGER DEFAULT 0"); }
	catch (const SQLite::Exception&) {}
	try { db_music_.exec("ALTER TABLE songs ADD COLUMN is_video INTEGER DEFAULT 0"); }
	catch (const SQLite::Exception&) {}
	try { db_music_.exec("ALTER TABLE songs ADD COLUMN width INTEGER DEFAULT 0"); }
	catch (const SQLite::Exception&) {}
	try { db_music_.exec("ALTER TABLE songs ADD COLUMN height INTEGER DEFAULT 0"); }
	catch (const SQLite::Exception&) {}
	try { db_music_.exec("ALTER TABLE songs ADD COLUMN video_codec TEXT"); }
	catch (const SQLite::Exception&) {}
	try { db_music_.exec("ALTER TABLE songs ADD COLUMN audio_codec TEXT"); }
	catch (const SQLite::Exception&) {}
	try { db_music_.exec("ALTER TABLE folders ADD COLUMN content_type TEXT"); }
	catch (const SQLite::Exception&) {}
	try { db_music_.exec("ALTER TABLE songs ADD COLUMN cover_path TEXT"); }
	catch (const SQLite::Exception&) {}

	// Backfill song_artists from album_artists for any songs that were scanned
	// before this link was introduced.
	db_music_.exec(
		"INSERT OR IGNORE INTO song_artists (song_id, artist_id, role)"
		" SELECT s.id, aa.artist_id, 'artist'"
		" FROM songs s"
		" JOIN album_artists aa ON aa.album_id = s.album_id AND aa.role = 'albumartist'"
		);
	}

void MediaStore::scan()
	{
	// The API key is a stored setting, not a start-up argument, so it is read
	// here rather than in the constructor: entering it in the client takes
	// effect on the next scan without a restart.
	tmdb_.set_api_key(get_setting("tmdb_key"));

	// The uploads root holds per-user personal files, not library content, so
	// it is never walked here — that is what replaced the old ".users" hidden
	// directory sitting inside the music tree.
	for (const auto& root : roots_) {
		if (root.cfg.type == "uploads") continue;
		std::cout << stamp() << "Scan started: " << root.cfg.name
		          << " (" << root.cfg.path << ")" << std::endl;

		// Ensure the root folder row exists before per-artist work begins.
		// Its stored path is the root *name*, which is what makes every path
		// below it self-identifying.
		{
		std::lock_guard<std::mutex> lock(db_mutex_);
		SQLite::Transaction txn(db_music_);
		upsert_folder(fs::path(root.cfg.path), -1);
		txn.commit();
		}

		// Collect paths to scan (absolute, since scan_artist_dir needs absolute
		// for fs ops): level-1 dirs present on disk, plus any still in the DB
		// so deleted ones get pruned.
		std::set<fs::path> to_scan;
		std::error_code ec;
		for (auto& e : fs::directory_iterator(root.cfg.path, ec)) {
			// Skip hidden directories. Nothing dot-prefixed at the top of a
			// library is an artist: a leftover .users from before uploads got
			// their own root, .stfolder/.stversions on a synced tree, @eaDir
			// on a Synology share. Indexing them adds junk entries and churns
			// every folders.id after them on each rescan.
			if (!e.is_directory()) continue;
			auto name = e.path().filename().string();
			if (!name.empty() && name.front() == '.') continue;
			to_scan.insert(e.path());
			}
		if (ec)
			std::cout << stamp() << "Scan: cannot read " << root.cfg.path
			          << ": " << ec.message() << std::endl;
		{
		std::lock_guard<std::mutex> lock(db_mutex_);
		SQLite::Statement s(db_music_,
			"SELECT path FROM folders"
			" WHERE parent_id = (SELECT id FROM folders WHERE path = ?)");
		s.bind(1, root.cfg.name);
		while (s.executeStep())
			to_scan.insert(fs::path(join_root(s.getColumn(0).getString())));
		}

		// Process each level-1 dir in its own transaction so db_mutex_ is
		// released between them and API handlers stay responsive.
		for (auto& artist_path : to_scan)
			scan_artist_dir(artist_path);

		scan_root_files(root);
		}

	std::cout << stamp() << "Scan complete" << std::endl;
	}

void MediaStore::scan_dirs(const std::set<std::string>& dirs)
	{
	tmdb_.set_api_key(get_setting("tmdb_key"));

	// dirs are stored-form paths ("<root>/<level-1 dir>").  A bare root name,
	// or anything that does not resolve, means we lost track of what changed
	// (queue overflow, unknown root) and the safe answer is a full scan.
	for (auto& d : dirs) {
		std::string abs = join_root(d);
		if (abs.empty() || d.find('/') == std::string::npos) {
			scan();
			return;
			}
		}
	// Known to the DB as a folder — which is what tells a section that has just
	// been deleted apart from a file that never was one.
	auto is_known_folder = [&](const std::string& rel) {
		std::lock_guard<std::mutex> lock(db_mutex_);
		SQLite::Statement q(db_music_, "SELECT 1 FROM folders WHERE path = ?");
		q.bind(1, rel);
		return q.executeStep();
		};

	for (auto& d : dirs) {
		fs::path abs(join_root(d));
		// A depth-1 entry that is neither a directory now nor a folder in the
		// DB is a loose file sitting in the root itself — added, changed or
		// deleted.  The watcher reports the file, since there is no directory
		// between it and the root.
		if (!fs::is_directory(abs) && !is_known_folder(d)) {
			const RootRec* r = root_for_rel(d);
			// The uploads root is per-user space, never library content, and
			// scan() does not walk it either.
			if (r && r->cfg.type != "uploads")
				scan_root_files(*r);
			continue;
			}
		scan_artist_dir(abs);
		}
	}

// Per-song data collected in Phases 1–3, consumed in Phase 4.
struct SongReadData {
	std::string path;
	std::string folder_path;   // disc dir or album dir
	int64_t     mtime       = 0;
	int64_t     file_size   = 0;
	std::string codec;
	std::string cover;         // sidecar image beside the file; "" for none
	int         disc_number = 0;
	bool        changed     = false;
	bool        is_video    = false;
	// Video only, from the filename parse in Phase 1. Season is stored
	// nowhere — disc numbers are positional — but Phase 3c needs it to know
	// whether to ask TMDB about a film or about a series.
	int         season      = 0;
	// populated in Phase 3 only when changed == true:
	std::string title;
	int         track_nr    = 0;
	int         year        = 0;
	std::string genre;
	double      duration    = 0.0;
	int         bitrate     = 0;
	int         sr          = 0;
	int         channels    = 0;
	// video only; all zero/empty for audio rows:
	int         width       = 0;
	int         height      = 0;
	std::string video_codec;
	std::string audio_codec;
	// DVD rips only: the ordered VOBs of one titleset, discovered in Phase 1
	// so Phase 3 does not have to rediscover them. `path` is the first of
	// these. Empty for everything else, which is what marks a row as ordinary.
	std::vector<std::string> parts;
	};

struct AlbumReadData {
	std::string               path;
	std::string               title;
	std::string               cover;
	std::vector<std::string>  disc_paths;  // sorted; index+1 = disc number
	std::vector<SongReadData> songs;
	// Filled by Phase 3c when TMDB identified this album, consumed by the
	// album transaction in Phase 4 — which is where the folder id, and so the
	// row to attach a description to, finally exists.
	std::string               tmdb_title;
	int                       tmdb_year = 0;
	std::string               overview;
	};

// One media file → everything Phase 1 can learn about it without a lock.
static SongReadData read_song_file(const fs::path& p,
                                    const std::string& folder_path,
                                    int disc_number)
	{
	SongReadData sdat;
	sdat.path        = p.string();
	sdat.folder_path = folder_path;
	sdat.mtime       = mtime_of(p);
	sdat.file_size   = static_cast<int64_t>(fs::file_size(p));
	sdat.codec       = p.extension().string().substr(1);
	std::transform(sdat.codec.begin(), sdat.codec.end(),
	               sdat.codec.begin(), ::tolower);
	sdat.cover       = find_song_cover(p);
	sdat.is_video    = is_video_file(p);
	sdat.disc_number = disc_number;

	// A video's title comes from its filename, and that parse happens here in
	// Phase 1 rather than in Phase 3 with the other metadata reads.  Two
	// reasons: it is pure string work with no I/O, so it costs nothing to do
	// for every file; and Phase 3 runs only for *changed* files, so a parse
	// living there would never reach a video already in the database.  The
	// guarded UPDATE in upsert_song_with_data() is what carries it to those.
	//
	// The folder and the one above it are passed because the title is often on
	// the folder rather than on the file — "The Third Man (1949)/title00.mkv" —
	// and because a "Season 01" folder means the show's name is one level up.
	if (sdat.is_video) {
		auto vn = resolve_video_name(
			p.stem().string(),
			p.parent_path().filename().string(),
			p.parent_path().parent_path().filename().string());
		// A name that is nothing but a year, in a folder that says no more,
		// parses to an empty title.  The raw stem is a poor title but an empty
		// one is worse, and every caller below assumes there is something.
		sdat.title    = vn.title.empty() ? p.stem().string() : vn.title;
		sdat.year     = vn.year;
		sdat.track_nr = vn.episode;
		sdat.season   = vn.season;
		}
	return sdat;
	}

// Phase 3 for one changed file: the slow reads, with no lock held.  Shared by
// scan_artist_dir() and scan_root_files().
static void read_song_metadata(SongReadData& sdat)
	{
	// A DVD titleset: dimensions and codecs are identical across the parts,
	// but the duration is not — probing only the first would report a 1 GB
	// fragment's length as the whole title, so sum them.  Title and track
	// number came from the titleset number in Phase 1 and must not be
	// overwritten by the filename-based rules below.
	if (!sdat.parts.empty()) {
		for (const auto& part : sdat.parts) {
			auto vp = probe_video(part);
			if (!vp) {
				std::cout << stamp() << "scan: ffprobe failed for "
				          << part << std::endl;
				continue;
				}
			sdat.duration += vp->duration;
			if (sdat.width == 0) {
				sdat.width       = vp->width;
				sdat.height      = vp->height;
				sdat.video_codec = vp->video_codec;
				sdat.audio_codec = vp->audio_codec;
				sdat.bitrate     = vp->bitrate;
				}
			}
		return;
		}

	// Video takes a different reader entirely: TagLib cannot open these
	// containers, so ffprobe supplies duration, bitrate and dimensions.  Title,
	// year and episode number came from the filename back in Phase 1 — see
	// read_song_file().  A probe failure still leaves a usable row.
	if (sdat.is_video) {
		if (auto vp = probe_video(sdat.path)) {
			sdat.duration    = vp->duration;
			sdat.bitrate     = vp->bitrate;
			sdat.width       = vp->width;
			sdat.height      = vp->height;
			sdat.video_codec = vp->video_codec;
			sdat.audio_codec = vp->audio_codec;
			}
		else
			std::cout << stamp() << "scan: ffprobe failed for "
			          << sdat.path << std::endl;
		return;
		}

	TagLib::FileStream stream(sdat.path.c_str(), true /* readOnly */);
	TagLib::FileRef    f(&stream);
	sdat.title = fs::path(sdat.path).stem().string();

	if (!f.isNull() && f.tag()) {
		auto* t = f.tag();
		if (!t->title().isEmpty())
			sdat.title = t->title().toCString(true);
		sdat.track_nr = static_cast<int>(t->track());
		sdat.year     = static_cast<int>(t->year());
		if (!t->genre().isEmpty())
			sdat.genre = t->genre().toCString(true);
		}
	if (sdat.disc_number == 0 && !f.isNull()) {
		auto props = f.file()->properties();
		auto it    = props.find("DISCNUMBER");
		if (it != props.end() && !it->second.isEmpty()) {
			try { sdat.disc_number = it->second.front().toInt(); }
			catch (...) {}
			}
		}
	if (!f.isNull() && f.audioProperties()) {
		auto* ap      = f.audioProperties();
		sdat.duration = ap->lengthInSeconds();
		sdat.bitrate  = ap->bitrate();
		sdat.sr       = ap->sampleRate();
		sdat.channels = ap->channels();
		}
	}

// A film's title is as often on the folder as on the file — "The.Third.Man.
// 1949.1080p.BluRay/movie.mkv" — so the album name gets the same treatment the
// song titles get.
//
// Only for albums that actually hold video.  A music album folder legitimately
// named "Album (2017)" would otherwise lose its year, and that year is not
// junk there.
//
// Guarded the same way song titles are, against the folder name as stored:
// upsert_album() is INSERT OR IGNORE, so an existing album's title is never
// rewritten by the scan today, and a title somebody edited must keep that
// property.
// A TMDB match, when there is one, supersedes the filename parse — that is the
// whole point of asking, and it is what turns "THE.THIRD.MAN.1949" into "The
// Third Man". The guard is unchanged either way.
static void apply_album_video_name(SQLite::Database& db, int album_id,
                                    const std::string& stored_title,
                                    const std::vector<SongReadData>& songs,
                                    const std::string& tmdb_title = "",
                                    int tmdb_year = 0)
	{
	if (std::none_of(songs.begin(), songs.end(),
	        [](const SongReadData& s) { return s.is_video; }))
		return;

	std::string title = tmdb_title;
	int         year  = tmdb_year;
	if (title.empty()) {
		VideoName vn = parse_video_name(stored_title);
		if (!vn.cleaned || vn.title.empty()) return;
		title = vn.title;
		year  = vn.year;
		}
	if (title == stored_title) return;

	SQLite::Statement upd(db,
		"UPDATE albums SET title = ?,"
		"                  year = CASE WHEN year = 0 THEN ? ELSE year END"
		" WHERE id = ? AND title = ?");
	upd.bind(1, title);
	upd.bind(2, year);
	upd.bind(3, album_id);
	upd.bind(4, stored_title);
	upd.exec();
	}

// A film's plot goes where an album's liner notes go, which is why this needed
// no new endpoint and no web-client change: getAlbumInfo2 already reads this
// table and the album view already renders `notes`.
//
// ON CONFLICT rather than INSERT OR REPLACE so a row a MusicBrainz lookup
// already created keeps its other columns. Called from inside the album
// transaction, so it takes the database directly — cache_album_info() would
// deadlock, taking db_mutex_ a second time on a non-recursive mutex.
static void apply_album_overview(SQLite::Database& db, int folder_id,
                                  const std::string& overview)
	{
	if (overview.empty()) return;
	SQLite::Statement ins(db,
		"INSERT INTO album_info_cache (folder_id, notes) VALUES (?, ?)"
		" ON CONFLICT(folder_id) DO UPDATE SET notes = excluded.notes");
	ins.bind(1, folder_id);
	ins.bind(2, overview);
	ins.exec();
	}

// Phase 3b for one album's worth of files: manufacture cover art for the videos
// that have none.  Returns the path whose art should also serve as the album's,
// or "" — chosen by sort order rather than by re-running find_cover(), whose
// pass 3 takes whatever directory_iterator hands it first, so a season folder
// would otherwise get a different episode's frame on different machines.
//
// It runs for *unchanged* songs too.  The guard is "has no cover and has no
// cached image", not sdat.changed, so switching the feature on back-fills an
// existing library on the next scan without anything having to be re-tagged or
// touched — the unchanged-song UPDATE writes cover_path unconditionally for
// exactly this class of reason.
//
// The image is written immediately rather than carried into Phase 4: a season
// of two dozen episodes would otherwise hold a couple of megabytes of JPEG in
// SongReadData for the whole artist.  Nothing in Phase 4 depends on it; what
// Phase 4 needs is the cover_path this leaves behind.
static std::string make_video_art_songs(
	MediaStore& store, const VideoArt& art, std::vector<SongReadData>& songs,
	const std::unordered_map<std::string, int64_t>& known)
	{
	std::string album_art;

	for (auto& sdat : songs) {
		// A sidecar image beside the file always wins — this tier exists only
		// for the files that have nothing at all.
		if (!sdat.is_video || !sdat.cover.empty()) continue;

		std::string rel = store.rel_path(sdat.path);
		auto        it  = known.find(rel);
		if (it == known.end() || it->second != sdat.mtime) {
			// A DVD titleset is one stream split across VOBs, so the whole
			// concat: list is the input — seeking into the first part alone
			// would land inside a 1 GB fragment rather than inside the film.
			std::string input;
			if (!sdat.parts.empty()) input = dvd_input(sdat.parts.front());

			auto result = art.generate(sdat.path, input);
			if (!result) {
				std::cout << stamp() << "video art: nothing usable in " << rel
				          << std::endl;
				continue;
				}
			store.store_video_art(rel, sdat.mtime, result->mime,
			                      result->source, result->bytes);
			std::cout << stamp() << "video art: " << result->source << ", "
			          << result->bytes.size() << " bytes for " << rel
			          << std::endl;
			}

		// cover_path names the media file itself, which is what makes every
		// existing cover-art query and the whole web client work unchanged.
		// See MediaStore::VideoArtRow.
		sdat.cover = sdat.path;
		if (album_art.empty() || sdat.path < album_art) album_art = sdat.path;
		}

	return album_art;
	}

// The whole of Phase 3b for one artist.  Shared with scan_root_files(), which
// has no AlbumReadData and calls make_video_art_songs() directly.
static void make_video_art(MediaStore& store, const VideoArt& art,
                           std::vector<AlbumReadData>& albums,
                           const std::string& prefix)
	{
	auto known = store.load_video_art_keys(prefix);
	for (auto& adat : albums) {
		std::string album_art = make_video_art_songs(store, art, adat.songs,
		                                              known);
		// An album that already has an image keeps it: a hand-placed poster,
		// or one uploaded through setCoverArt, outranks a frame grab.
		if (adat.cover.empty() && !album_art.empty()) adat.cover = album_art;
		}
	}

// ---- Phase 3c: identify the video online ------------------------------

// An 'error' row is a network failure, which is temporary by nature. An
// 'unmatched' one is a judgement about the name, and re-asking tomorrow would
// get the same answer — only a rename changes it, and a rename changes the
// stored query, which re-asks anyway.
static constexpr int64_t TMDB_ERROR_RETRY_S = 24 * 60 * 60;

// What was asked, stored so a rename can be detected as a different question.
static std::string tmdb_query_key(const std::string& title, int year, bool tv)
	{
	return title + "|" + std::to_string(year) + "|" + (tv ? "tv" : "movie");
	}

// One lookup: consults the cache, asks TMDB only when it has to, and records
// the outcome either way. Returns the row to act on, matched or not.
static MediaStore::VideoMetaRow tmdb_lookup(
	MediaStore& store, const Tmdb& tmdb, const std::string& rel_key,
	const std::string& title, int year, bool tv, int explicit_id)
	{
	std::string query = tmdb_query_key(title, year, tv);

	if (auto cached = store.get_video_meta(rel_key)) {
		bool stale = cached->status == "error"
		    && std::time(nullptr) - cached->fetched_at > TMDB_ERROR_RETRY_S;
		if (cached->query == query && !stale) return *cached;
		}

	MediaStore::VideoMetaRow row;
	row.query      = query;
	row.media_type = tv ? "tv" : "movie";

	std::optional<TmdbMatch> m;
	if (explicit_id > 0) m = tmdb.by_id(explicit_id, tv);
	else                 m = tmdb.search(title, year, tv);

	if (m) {
		row.status      = "matched";
		row.tmdb_id     = m->id;
		row.title       = m->title;
		row.year        = m->year;
		row.overview    = m->overview;
		row.poster_path = m->poster_path;
		std::cout << stamp() << "tmdb: " << rel_key << " -> " << m->title
		          << " (" << m->year << ") id=" << m->id << std::endl;
		}
	else {
		// A network failure and a considered rejection are recorded
		// differently because they expire differently. There is no way to
		// tell them apart from here, so anything with no key configured or no
		// answer at all counts as an error and will be retried.
		row.status = tmdb.configured() ? "unmatched" : "error";
		std::cout << stamp() << "tmdb: no match for " << rel_key
		          << " (\"" << title << "\""
		          << (year ? " " + std::to_string(year) : "") << ")"
		          << std::endl;
		}

	store.store_video_meta(rel_key, row);
	return row;
	}

// Fetches the poster for a matched row unless there is already art for that
// path, and returns true when art exists afterwards either way. Keeping the
// poster keyed on a *song* path is what lets getCoverArt stay untouched: it
// resolves cover_path -> video_art exactly as it does for an embedded cover.
static bool tmdb_fetch_poster(MediaStore& store, const Tmdb& tmdb,
                               const MediaStore::VideoMetaRow& row,
                               const std::string& rel_song, int64_t mtime)
	{
	if (row.poster_path.empty()) return false;
	if (store.get_video_art(rel_song)) return true;

	auto bytes = tmdb.poster(row.poster_path);
	if (!bytes) return false;
	store.store_video_art(rel_song, mtime, "image/jpeg", "tmdb", *bytes);
	std::cout << stamp() << "tmdb: poster " << bytes->size() << " bytes for "
	          << rel_song << std::endl;
	return true;
	}

// Phase 3c for one artist or root.
//
// **One lookup per album, not per file.** A film is a folder, so
// "Movies/The Third Man (1949)/" is one question however many parts the film
// was split into. The exception is the loose-file album — the section holding
// makingcheese.mp4 next to other films — where each file is its own work; that
// album is the one whose path is the artist folder's, the same test the
// folder-parenting rule uses.
//
// Only the poster download is skipped when local art already exists. The
// description is wanted either way, and skipping the whole lookup would make
// an embedded thumbnail cost the film its plot.
static void lookup_video_meta(MediaStore& store, const Tmdb& tmdb,
                              std::vector<AlbumReadData>& albums,
                              const std::string& artist_path)
	{
	for (auto& adat : albums) {
		// The first video in sort order: the one whose path carries the album's
		// art, matching what Phase 3b picks for the same reason.
		const SongReadData* first = nullptr;
		bool                is_tv = false;
		for (const auto& s : adat.songs) {
			if (!s.is_video) continue;
			if (s.season > 0) is_tv = true;
			if (!first || s.path < first->path) first = &s;
			}
		if (!first) continue;

		bool loose = adat.path == artist_path;

		if (!loose) {
			VideoName vn = parse_video_name(adat.title);
			std::string title = vn.title.empty() ? adat.title : vn.title;
			int explicit_id = 0;
			if (!vn.tmdb_id.empty()) { try { explicit_id = std::stoi(vn.tmdb_id); }
			                           catch (...) {} }

			auto row = tmdb_lookup(store, tmdb, store.rel_path(adat.path),
			                        title, vn.year, is_tv, explicit_id);
			if (row.status != "matched") continue;

			adat.tmdb_title = row.title;
			adat.tmdb_year  = row.year;
			adat.overview   = row.overview;
			if (adat.cover.empty()
			        && tmdb_fetch_poster(store, tmdb, row,
			                              store.rel_path(first->path),
			                              first->mtime))
				adat.cover = first->path;
			continue;
			}

		// Loose files: each is its own work, so each is its own question.
		for (auto& sdat : adat.songs) {
			if (!sdat.is_video) continue;
			std::string rel = store.rel_path(sdat.path);
			auto row = tmdb_lookup(store, tmdb, rel, sdat.title, sdat.year,
			                        sdat.season > 0, 0);
			if (row.status != "matched") continue;
			if (!row.title.empty()) {
				sdat.title = row.title;
				if (row.year > 0) sdat.year = row.year;
				}
			if (sdat.cover.empty()
			        && tmdb_fetch_poster(store, tmdb, row, rel, sdat.mtime))
				sdat.cover = sdat.path;
			}
		}
	}

// Writes one song row; all slow I/O has already happened.
// Caller must hold db_mutex_ and an open transaction. sdat.path is absolute
// (Phase 1/3 use it for TagLib I/O); rel_path is its stored form, which the
// caller computes because strip_root() is a member and this is not.
static void upsert_song_with_data(SQLite::Database& db, const SongReadData& sdat,
                                   int album_id, int folder_id, int artist_id,
                                   const std::string& rel_path,
                                   const std::string& rel_cover)
	{
	if (!sdat.changed) {
		// An unchanged song still has to say it was seen: the per-album prune
		// below deletes whatever is left marked unvisited.  Disc number and
		// sidecar cover come from the folder layout, not from the file, so
		// they can change while the file itself does not.
		SQLite::Statement upd(db,
			"UPDATE songs SET last_scanned = CURRENT_TIMESTAMP,"
			"                 disc_number = CASE WHEN ? > 0 THEN ? ELSE disc_number END,"
			"                 cover_path = ?"
			" WHERE path = ?");
		upd.bind(1, sdat.disc_number);
		upd.bind(2, sdat.disc_number);
		upd.bind(3, rel_cover);
		upd.bind(4, rel_path);
		upd.exec();

		// A video's title is derived from its filename, so it can improve
		// without the file changing — which is how a library scanned before
		// the parser existed gets back-filled.
		//
		// Guarded, and the guard is the point: the title is overwritten only
		// while it is still one of the two things the scanner itself could
		// have put there — the raw stem, or the raw stem with the old leading
		// track-number strip applied.  Anything else was typed by a person
		// through updateSong, and for video that edit exists *only* in the
		// database: TagLib cannot write these containers, so there is no tag
		// to re-read and a clobber would be unrecoverable.
		if (sdat.is_video && !sdat.title.empty()) {
			static const std::regex old_prefix(R"(^\d+[. -]+)");
			std::string stem = fs::path(sdat.path).stem().string();
			SQLite::Statement t(db,
				"UPDATE songs SET title = ?,"
				"                 year = CASE WHEN year = 0 THEN ? ELSE year END,"
				"                 track_number = CASE WHEN ? > 0 THEN ?"
				"                                ELSE track_number END"
				" WHERE path = ? AND title IN (?, ?)");
			t.bind(1, sdat.title);
			t.bind(2, sdat.year);
			t.bind(3, sdat.track_nr);
			t.bind(4, sdat.track_nr);
			t.bind(5, rel_path);
			t.bind(6, stem);
			t.bind(7, std::regex_replace(stem, old_prefix, ""));
			t.exec();
			}
		return;
		}

	// Audio only.  A video title has already been through the filename parser,
	// which takes a leading number as an episode number without touching the
	// title — applying this to it as well would turn "12 Angry Men" into
	// "Angry Men".
	static const std::regex track_prefix(R"(^\d+[. -]+)");
	std::string title = sdat.is_video
	    ? sdat.title : std::regex_replace(sdat.title, track_prefix, "");

	SQLite::Statement ins(db,
		"INSERT OR REPLACE INTO songs"
		" (album_id, folder_id, path, filename, title, track_number, disc_number,"
		"  year, genre, duration, bitrate, sample_rate, channels, codec,"
		"  file_size, file_modified, is_video, width, height, video_codec,"
		"  audio_codec, cover_path, last_scanned)"
		" VALUES (?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,CURRENT_TIMESTAMP)");
	ins.bind(1,  album_id);
	ins.bind(2,  folder_id);
	ins.bind(3,  rel_path);
	ins.bind(4,  fs::path(sdat.path).filename().string());
	ins.bind(5,  title);
	ins.bind(6,  sdat.track_nr);
	ins.bind(7,  sdat.disc_number > 0 ? sdat.disc_number : 1);
	ins.bind(8,  sdat.year);
	ins.bind(9,  sdat.genre);
	ins.bind(10, sdat.duration);
	ins.bind(11, sdat.bitrate);
	ins.bind(12, sdat.sr);
	ins.bind(13, sdat.channels);
	ins.bind(14, sdat.codec);
	ins.bind(15, sdat.file_size);
	ins.bind(16, sdat.mtime);
	ins.bind(17, sdat.is_video ? 1 : 0);
	ins.bind(18, sdat.width);
	ins.bind(19, sdat.height);
	ins.bind(20, sdat.video_codec);
	ins.bind(21, sdat.audio_codec);
	ins.bind(22, rel_cover);
	ins.exec();

	int song_id = static_cast<int>(db.getLastInsertRowid());
	SQLite::Statement lnk(db,
		"INSERT OR IGNORE INTO song_artists (song_id, artist_id, role)"
		" VALUES (?, ?, 'artist')");
	lnk.bind(1, song_id);
	lnk.bind(2, artist_id);
	lnk.exec();
	}

// Targeted rescan of one artist directory.  Same mark→walk→prune pattern as
// scan(), but scoped to a single artist subtree so most of the library is
// untouched.
//
// Structured in four phases so slow filesystem I/O never holds db_mutex_:
//   1. Walk disk — collect album/song paths, mtimes, file sizes (no lock)
//   2. Brief read lock — fetch known mtimes to identify changed files
//   3. TagLib (audio) or ffprobe (video) reads for changed files only (no lock)
//   4. Short write txns: one per album, plus the unvisited-mark and the
//      artist-level prune.  Per-album commits keep the mutex hold time
//      bounded so REST handlers stay responsive during the scan.
void MediaStore::scan_artist_dir(const fs::path& artist_path)
	{
	// SQL `path LIKE ?` queries below compare against the stored-form column,
	// so the prefix must be stored-form too (e.g. "music/Artist Name/%").
	std::string prefix =
		strip_root(artist_path.string()) + "/%";
	bool exists = fs::is_directory(artist_path);

	// The owning root, needed to re-create the root folder row below. A path
	// outside every root cannot be scanned — refuse rather than inventing one.
	const RootRec* root = root_for_abs(artist_path.string());
	if (!root) {
		std::cout << stamp() << "Rescan: ignoring path outside every root: "
		          << artist_path << std::endl;
		return;
		}
	const std::string& root_path = root->cfg.path;

	std::cout << stamp() << "Rescan: " << artist_path.filename().string()
	          << (exists ? "" : " (removed)") << std::endl;

	// ---- Phase 1: walk disk (no lock) ----
	std::vector<AlbumReadData> albums;
	bool had_loose = false;   // media files directly in the artist folder
	if (exists) {
		for (auto& album_entry : fs::directory_iterator(artist_path)) {
			if (!album_entry.is_directory()) continue;

			AlbumReadData adat;
			adat.path  = album_entry.path().string();
			adat.title = album_entry.path().filename().string();
			std::replace(adat.title.begin(), adat.title.end(), '_', ' ');
			adat.cover = find_cover(album_entry.path());

			// A DVD rip is one track per titleset and nothing else.  This has
			// to short-circuit the normal enumeration below: otherwise
			// VIDEO_TS becomes a disc subdirectory and every menu VOB becomes
			// a track of its own.
			if (auto video_ts = dvd_video_ts_dir(album_entry.path());
			        !video_ts.empty()) {
				for (auto& [ts, vobs] : dvd_titlesets(video_ts)) {
					int64_t total = 0;
					int64_t newest = 0;
					std::error_code fec;
					for (auto& v : vobs) {
						total  += static_cast<int64_t>(fs::file_size(v, fec));
						newest  = std::max(newest, mtime_of(v));
						}
					// A few seconds of DVD video is reliably an FBI warning or
					// a studio logo, never something worth a row.
					if (total < 10 * 1024 * 1024) continue;

					SongReadData sdat;
					sdat.path        = vobs.front().string();
					sdat.folder_path = adat.path;   // the album, not VIDEO_TS
					sdat.mtime       = newest;
					sdat.file_size   = total;
					sdat.codec       = "vob";
					sdat.is_video    = true;
					sdat.disc_number = 0;
					// DVD titles carry no names, only numbers.
					sdat.title       = "Title " + std::to_string(ts);
					sdat.track_nr    = ts;
					for (auto& v : vobs) sdat.parts.push_back(v.string());
					adat.songs.push_back(std::move(sdat));
					}
				albums.push_back(std::move(adat));
				continue;
				}

			std::vector<fs::directory_entry> disc_dirs;
			std::vector<fs::path>            direct_files;
			for (auto& e : fs::directory_iterator(album_entry.path())) {
				if      (e.is_directory())                               disc_dirs.push_back(e);
				else if (e.is_regular_file() && is_media_file(e.path())) direct_files.push_back(e.path());
				}
			std::sort(disc_dirs.begin(), disc_dirs.end(),
				[](const fs::directory_entry& a, const fs::directory_entry& b) {
					return a.path().filename() < b.path().filename();
					});

			for (auto& de : disc_dirs)
				adat.disc_paths.push_back(de.path().string());

			int disc_count = (int)disc_dirs.size();
			for (int dn = 0; dn < disc_count; ++dn) {
				for (auto& te : fs::directory_iterator(disc_dirs[dn].path())) {
					if (!te.is_regular_file() || !is_media_file(te.path())) continue;
					adat.songs.push_back(read_song_file(
						te.path(), disc_dirs[dn].path().string(), dn + 1));
					}
				}
			for (auto& p : direct_files)
				adat.songs.push_back(read_song_file(p, adat.path, 0));

			albums.push_back(std::move(adat));
			}

		// Media files sitting directly in the artist/section folder, with no
		// folder of their own — one documentary, one home video, one clip.
		// The rule is Subsonic's and Airsonic's: a folder that directly
		// contains media is itself an album.  So the section becomes an album
		// alongside the albums below it, and does not stop being their parent.
		AlbumReadData loose;
		for (auto& e : fs::directory_iterator(artist_path)) {
			if (!e.is_regular_file() || !is_media_file(e.path())) continue;
			// "._movie.mp4" is an AppleDouble resource fork, not a film.
			auto name = e.path().filename().string();
			if (!name.empty() && name.front() == '.') continue;
			loose.songs.push_back(read_song_file(e.path(), artist_path.string(), 0));
			}
		if (!loose.songs.empty()) {
			loose.path  = artist_path.string();
			loose.title = artist_path.filename().string();
			std::replace(loose.title.begin(), loose.title.end(), '_', ' ');
			// Non-recursive: this folder's subdirectories are other albums,
			// and their covers are not this one's.
			loose.cover = find_cover(artist_path, false);
			albums.push_back(std::move(loose));
			had_loose = true;
			}
		}

	// ---- Phase 2: brief read lock — identify changed files ----
	// known_mtimes is keyed by the same absolute-path form as sdat.path so the
	// per-song lookup below stays a direct comparison. The DB column is
	// relative; compose absolute via join_root() on the way in.
	std::unordered_map<std::string, int64_t> known_mtimes;
	{
	std::lock_guard<std::mutex> lock(db_mutex_);
	SQLite::Statement q(db_music_,
		"SELECT path, file_modified FROM songs WHERE path LIKE ?");
	q.bind(1, prefix);
	while (q.executeStep())
		known_mtimes[join_root(q.getColumn(0).getString())]
			= q.getColumn(1).getInt64();
	}

	for (auto& adat : albums)
		for (auto& sdat : adat.songs) {
			auto it = known_mtimes.find(sdat.path);
			sdat.changed = (it == known_mtimes.end() || it->second != sdat.mtime);
			}

	// ---- Phase 3: TagLib reads for changed files only (no lock) ----
	for (auto& adat : albums)
		for (auto& sdat : adat.songs) {
			if (!sdat.changed) continue;

			// Defence-in-depth: refuse to open any file that — after symlink
			// resolution — sits outside every root.
			if (!path_is_within_root(sdat.path)) {
				std::cout << stamp() << "scan: skipping file outside every root: "
				          << sdat.path << std::endl;
				continue;
				}
			read_song_metadata(sdat);
			}

	// ---- Phase 3b: cover art for videos that have none (no lock) ----
	if (video_art_) make_video_art(*this, *video_art_, albums, prefix);

	// ---- Phase 3c: identify films and series online (no lock) ----
	// Only under a categories root. A concert or a music video sitting under a
	// performer stays local: both layouts are L1/L2/files and the scanner
	// cannot tell a concert film from a documentary by shape, which is the
	// same reason is_category_folder() exists — pointed the other way.
	if (tmdb_.configured() && root->cfg.type == "categories")
		lookup_video_meta(*this, tmdb_, albums, artist_path.string());

	// ---- Phase 4: short write txns ----
	// Mark this artist's subfolders as unvisited.  Folders not re-stamped
	// by an album commit below are pruned at the end of the artist.
	{
	std::lock_guard<std::mutex> lock(db_mutex_);
	SQLite::Transaction txn(db_music_);
	SQLite::Statement s(db_music_,
		"UPDATE folders SET last_scanned = NULL WHERE path LIKE ?");
	s.bind(1, prefix);
	s.exec();
	txn.commit();
	}

	// Set when an album's commit failed, which makes the prune unsafe: the
	// prune deletes whatever is still marked unvisited, and a failed album is
	// indistinguishable from a deleted one by that mark alone.
	bool album_failed = false;

	if (exists) {
		int root_id, artist_folder_id, artist_id;
		{
		std::lock_guard<std::mutex> lock(db_mutex_);
		SQLite::Transaction txn(db_music_);
		root_id          = upsert_folder(fs::path(root_path), -1);
		artist_folder_id = upsert_folder(artist_path, root_id);
		artist_id        = upsert_artist(artist_path.filename().string());
		txn.commit();
		}

		for (auto& adat : albums) {
			// One album's transaction failing must not cost the rest of the
			// scan.  A full scan of a large library is minutes of work, and
			// the usual cause here is an external writer holding the lock for
			// longer than the busy timeout — momentary, and specific to this
			// commit.  Note the prune at the end of this function is then
			// skipped: a failed album leaves its folder row still marked
			// unvisited, and pruning on incomplete information would delete an
			// album that is present on disk.
			try {
			{
			std::lock_guard<std::mutex> lock(db_mutex_);
			SQLite::Transaction txn(db_music_);

			// The loose-file album *is* the artist folder, so it must not be
			// upserted with itself as its parent.
			int album_folder_id = adat.path == artist_path.string()
			                    ? artist_folder_id
			                    : upsert_folder(fs::path(adat.path), artist_folder_id);
			int album_id        = upsert_album(album_folder_id, adat.title, artist_id, 0, "");
			apply_album_video_name(db_music_, album_id, adat.title, adat.songs,
			                        adat.tmdb_title, adat.tmdb_year);
			apply_album_overview(db_music_, album_folder_id, adat.overview);

			if (!adat.cover.empty()) {
				SQLite::Statement upd(db_music_,
					"UPDATE albums SET cover_path = ? WHERE id = ?");
				upd.bind(1, strip_root(adat.cover));
				upd.bind(2, album_id);
				upd.exec();
				}

			// Build path→folder_id map for this album's disc dirs.
			std::unordered_map<std::string, int> fid_map;
			fid_map[adat.path] = album_folder_id;
			int disc_count = (int)adat.disc_paths.size();
			for (int dn = 0; dn < disc_count; ++dn) {
				int disc_fid = upsert_folder(fs::path(adat.disc_paths[dn]), album_folder_id);
				fid_map[adat.disc_paths[dn]] = disc_fid;
				}

			// Songs get the same mark→sweep the folders get, scoped to exactly
			// this album's folders.  The folder-level prune below cannot reach
			// a track deleted from an album that still exists, and it can
			// never reach the loose-file album at all, whose folder is the
			// artist folder and is always re-stamped.  Listing the ids rather
			// than "parent_id = album_folder_id" matters for that album: its
			// child folders are the section's *other* albums.
			std::string fid_list;
			for (auto& [_, fid] : fid_map) {
				if (!fid_list.empty()) fid_list += ",";
				fid_list += std::to_string(fid);
				}
			db_music_.exec(("UPDATE songs SET last_scanned = NULL"
			                " WHERE folder_id IN (" + fid_list + ")").c_str());

			for (auto& sdat : adat.songs) {
				auto fit = fid_map.find(sdat.folder_path);
				int  fid = (fit != fid_map.end()) ? fit->second : album_folder_id;
				upsert_song_with_data(db_music_, sdat, album_id, fid, artist_id,
				                       strip_root(sdat.path),
				                       sdat.cover.empty() ? "" : strip_root(sdat.cover));
				}

			db_music_.exec(("DELETE FROM songs WHERE last_scanned IS NULL"
			                " AND folder_id IN (" + fid_list + ")").c_str());
			if (int n = db_music_.getChanges(); n > 0)
				std::cout << stamp() << "  pruned " << n << " songs from "
				          << fs::path(adat.path).filename().string() << std::endl;

			if (disc_count > 1) {
				SQLite::Statement upd(db_music_, "UPDATE albums SET disc_count=? WHERE id=?");
				upd.bind(1, disc_count);
				upd.bind(2, album_id);
				upd.exec();
				}
			{
			SQLite::Statement upd(db_music_,
				"UPDATE albums SET year = ("
				"  SELECT year FROM songs WHERE album_id = ? AND year > 0"
				"  ORDER BY disc_number, track_number LIMIT 1"
				") WHERE id = ?");
			upd.bind(1, album_id);
			upd.bind(2, album_id);
			upd.exec();
			}

			txn.commit();
			}
			std::cout << stamp() << "  " << fs::path(adat.path).filename().string() << std::endl;
			}
			catch (const std::exception& e) {
				std::cout << stamp() << "  " << fs::path(adat.path).filename().string()
				          << ": skipped, " << e.what() << std::endl;
				album_failed = true;
				}
			}
		}

	// Prune stale entries within this artist's subtree.  Anything still
	// last_scanned IS NULL after the album commits above is genuinely gone —
	// but only if every album actually committed.  After a failure the mark
	// cannot distinguish "deleted from disk" from "we could not write it", so
	// pruning would remove an album that is still there.  Leave the subtree
	// alone and let the next scan sort it out.
	if (album_failed) {
		std::cout << stamp() << "  prune skipped: an album failed to commit"
		          << std::endl;
		return;
		}

	{
	std::lock_guard<std::mutex> lock(db_mutex_);
	SQLite::Transaction txn(db_music_);
	{
	SQLite::Statement s(db_music_,
		"DELETE FROM songs WHERE folder_id IN ("
		"  SELECT id FROM folders WHERE last_scanned IS NULL AND path LIKE ?"
		")");
	s.bind(1, prefix);
	s.exec();
	int n = db_music_.getChanges();
	if (n > 0)
		std::cout << stamp() << "  pruned " << n << " songs" << std::endl;
	}
	{
	// Keyed on "no song has this path any more" rather than on the folder
	// marks, so it also catches a single file deleted from an album that is
	// still there — the folder-level prune never reaches those.  Must run
	// after the songs delete above, in this same transaction.
	SQLite::Statement s(db_music_,
		"DELETE FROM video_art WHERE path LIKE ?"
		"  AND path NOT IN (SELECT path FROM songs)");
	s.bind(1, prefix);
	s.exec();
	}
	{
	SQLite::Statement s(db_music_,
		"DELETE FROM albums WHERE folder_id IN ("
		"  SELECT id FROM folders WHERE last_scanned IS NULL AND path LIKE ?"
		")");
	s.bind(1, prefix);
	s.exec();
	int n = db_music_.getChanges();
	if (n > 0)
		std::cout << stamp() << "  pruned " << n << " albums" << std::endl;
	}
	{
	SQLite::Statement s(db_music_,
		"DELETE FROM album_info_cache WHERE folder_id IN ("
		"  SELECT id FROM folders WHERE last_scanned IS NULL AND path LIKE ?"
		")");
	s.bind(1, prefix);
	s.exec();
	}
	{
	SQLite::Statement s(db_music_,
		"DELETE FROM artist_info_cache WHERE folder_id IN ("
		"  SELECT id FROM folders WHERE last_scanned IS NULL AND path LIKE ?"
		")");
	s.bind(1, prefix);
	s.exec();
	}
	{
	SQLite::Statement s(db_music_,
		"DELETE FROM folders WHERE last_scanned IS NULL AND path LIKE ?");
	s.bind(1, prefix);
	s.exec();
	int n = db_music_.getChanges();
	if (n > 0)
		std::cout << stamp() << "  pruned " << n << " folders" << std::endl;
	}
	{
	// video_meta is keyed by *either* an album folder or a song, so unlike
	// video_art above it cannot key its prune on songs alone — that would
	// delete every film, all of which are folders.  Runs after the folders
	// delete so a folder that went away this pass is already gone from the
	// set being checked against.
	SQLite::Statement s(db_music_,
		"DELETE FROM video_meta WHERE path LIKE ?"
		"  AND path NOT IN (SELECT path FROM songs)"
		"  AND path NOT IN (SELECT path FROM folders)");
	s.bind(1, prefix);
	s.exec();
	}
	// The loose-file album hangs off the artist folder itself, which is never
	// marked unvisited, so nothing above can reach it.  When the walk found no
	// loose files at all no album transaction ran for it either, so its songs
	// have to go from here; otherwise that transaction has already swept them.
	std::string artist_rel = strip_root(artist_path.string());
	if (!had_loose) {
		SQLite::Statement s(db_music_,
			"DELETE FROM songs"
			" WHERE folder_id = (SELECT id FROM folders WHERE path = ?)");
		s.bind(1, artist_rel);
		s.exec();
		if (int n = db_music_.getChanges(); n > 0)
			std::cout << stamp() << "  pruned " << n << " loose songs" << std::endl;
		}
	{
	SQLite::Statement s(db_music_,
		"DELETE FROM albums"
		" WHERE folder_id = (SELECT id FROM folders WHERE path = ?)"
		"   AND NOT EXISTS (SELECT 1 FROM songs WHERE songs.album_id = albums.id)");
	s.bind(1, artist_rel);
	s.exec();
	}
	{
	SQLite::Statement s(db_music_,
		"DELETE FROM artists WHERE id NOT IN"
		" (SELECT DISTINCT artist_id FROM album_artists)");
	s.exec();
	}
	if (!exists) {
		{
		SQLite::Statement s(db_music_,
			"DELETE FROM artist_info_cache"
			" WHERE folder_id = (SELECT id FROM folders WHERE path = ?)");
		s.bind(1, artist_rel);
		s.exec();
		}
		{
		SQLite::Statement s(db_music_,
			"DELETE FROM folders WHERE path = ?");
		s.bind(1, artist_rel);
		s.exec();
		if (db_music_.getChanges() > 0)
			std::cout << stamp() << "  pruned artist folder" << std::endl;
		}
		}
	txn.commit();
	}
	std::cout << stamp() << "Rescan complete" << std::endl;
	}

// Media files sitting directly in a root, with no section folder above them —
// a root used as one flat library, which is how Plex and Jellyfin organise a
// movie library.  Same rule as the loose files inside a section: the folder
// that directly contains media is itself an album.  Here the root row plays
// the part of both the artist and the album.
void MediaStore::scan_root_files(const RootRec& root)
	{
	// ---- Phase 1: walk the root itself (no lock) ----
	std::vector<SongReadData> songs;
	std::error_code ec;
	for (auto& e : fs::directory_iterator(root.cfg.path, ec)) {
		if (!e.is_regular_file() || !is_media_file(e.path())) continue;
		auto name = e.path().filename().string();
		if (!name.empty() && name.front() == '.') continue;
		songs.push_back(read_song_file(e.path(), root.cfg.path, 0));
		}
	if (ec) return;   // scan() has already reported the unreadable root

	// ---- Phase 2: brief read lock — identify changed files ----
	// Keyed by folder rather than by path prefix: "<root>/%" is the entire
	// root, and only the files directly in it belong to this album.
	std::unordered_map<std::string, int64_t> known_mtimes;
	{
	std::lock_guard<std::mutex> lock(db_mutex_);
	SQLite::Statement q(db_music_,
		"SELECT s.path, s.file_modified FROM songs s"
		" JOIN folders f ON f.id = s.folder_id WHERE f.path = ?");
	q.bind(1, root.cfg.name);
	while (q.executeStep())
		known_mtimes[join_root(q.getColumn(0).getString())]
			= q.getColumn(1).getInt64();
	}
	if (songs.empty() && known_mtimes.empty()) return;

	for (auto& sdat : songs) {
		auto it = known_mtimes.find(sdat.path);
		sdat.changed = (it == known_mtimes.end() || it->second != sdat.mtime);
		}

	// ---- Phase 3: metadata for changed files only (no lock) ----
	for (auto& sdat : songs) {
		if (!sdat.changed) continue;
		if (!path_is_within_root(sdat.path)) {
			std::cout << stamp() << "scan: skipping file outside every root: "
			          << sdat.path << std::endl;
			continue;
			}
		read_song_metadata(sdat);
		}

	// ---- Phase 3b: cover art for videos that have none (no lock) ----
	// The prefix takes in the whole root rather than just its loose files.
	// That over-fetches (path, mtime) pairs for the sections below, which is
	// cheap, and there is no narrower LIKE — "<root>/%" is the entire root,
	// which is the same reason this function exists at all.
	std::string root_art;
	if (video_art_) {
		auto known = load_video_art_keys(root.cfg.name + "/%");
		root_art   = make_video_art_songs(*this, *video_art_, songs, known);
		}

	// ---- Phase 3c: identify films online (no lock) ----
	// Every file directly in a root is a loose file, so this is always the
	// per-song case; wrapping them in a one-album vector reuses that branch
	// rather than repeating it. The root itself plays artist and album, so its
	// path is what marks the album as the loose one.
	if (tmdb_.configured() && root.cfg.type == "categories") {
		std::vector<AlbumReadData> one;
		one.push_back(AlbumReadData{});
		one.front().path  = root.cfg.path;
		one.front().songs = std::move(songs);
		lookup_video_meta(*this, tmdb_, one, root.cfg.path);
		songs = std::move(one.front().songs);
		}

	// ---- Phase 4: one write txn ----
	// A contended commit must not take the rest of the scan with it; the next
	// pass redoes this root's loose files from scratch anyway.
	try {
	std::lock_guard<std::mutex> lock(db_mutex_);
	SQLite::Transaction txn(db_music_);

	int         folder_id = upsert_folder(fs::path(root.cfg.path), -1);
	std::string fid       = std::to_string(folder_id);

	db_music_.exec(("UPDATE songs SET last_scanned = NULL WHERE folder_id = "
	                + fid).c_str());

	if (!songs.empty()) {
		int artist_id = upsert_artist(root.cfg.name);
		// No apply_album_video_name() here, unlike the album commit in
		// scan_artist_dir(): this album's title is the *root's name*, which is
		// configuration the user chose and a durable identifier, not a
		// filename that might be carrying release junk.
		int album_id  = upsert_album(folder_id, root.cfg.name, artist_id, 0, "");
		// Non-recursive: the subdirectories of a root are its sections, and
		// their covers are not this album's.
		std::string cover = find_cover(fs::path(root.cfg.path), false);
		if (cover.empty()) cover = root_art;
		if (!cover.empty()) {
			SQLite::Statement upd(db_music_,
				"UPDATE albums SET cover_path = ? WHERE id = ?");
			upd.bind(1, strip_root(cover));
			upd.bind(2, album_id);
			upd.exec();
			}
		for (auto& sdat : songs)
			upsert_song_with_data(db_music_, sdat, album_id, folder_id, artist_id,
			                       strip_root(sdat.path),
			                       sdat.cover.empty() ? "" : strip_root(sdat.cover));
		}

	db_music_.exec(("DELETE FROM songs WHERE last_scanned IS NULL"
	                " AND folder_id = " + fid).c_str());
	if (int n = db_music_.getChanges(); n > 0)
		std::cout << stamp() << "  pruned " << n << " loose songs from "
		          << root.cfg.name << std::endl;
	// Same rule as the artist prune: an image outlives its file only until the
	// next scan notices there is no song at that path any more.
	{
	SQLite::Statement s(db_music_,
		"DELETE FROM video_art WHERE path LIKE ?"
		"  AND path NOT IN (SELECT path FROM songs)");
	s.bind(1, root.cfg.name + "/%");
	s.exec();
	}
	{
	// Loose files in a root are looked up per song, so unlike the artist
	// prune there are no folder-keyed rows in this prefix — but the folders
	// clause costs nothing and keeps the two prunes reading the same way.
	SQLite::Statement s(db_music_,
		"DELETE FROM video_meta WHERE path LIKE ?"
		"  AND path NOT IN (SELECT path FROM songs)"
		"  AND path NOT IN (SELECT path FROM folders)");
	s.bind(1, root.cfg.name + "/%");
	s.exec();
	}
	// A root's album can only ever be the loose-file one, so it goes when the
	// last loose file does.
	db_music_.exec(("DELETE FROM albums WHERE folder_id = " + fid +
	                " AND NOT EXISTS (SELECT 1 FROM songs"
	                " WHERE songs.album_id = albums.id)").c_str());
	txn.commit();
	}
	catch (const std::exception& e) {
		std::cout << stamp() << "Scan: loose files in " << root.cfg.name
		          << ": skipped, " << e.what() << std::endl;
		}
	}

// ---- upsert helpers ---------------------------------------------------

int MediaStore::upsert_folder(const fs::path& path, int parent_id)
	{
	// Caller passes an absolute path (from fs walks); strip to stored form.
	// A root's own path strips to just its name, which is how a root row ends
	// up with path == name and no parent.
	std::string path_str = strip_root(path.string());
	std::string name     = path.filename().string();
	if (name.empty()) name = path.string();  // trailing-slash paths

	if (parent_id < 0) {
		SQLite::Statement ins(db_music_,
			"INSERT OR IGNORE INTO folders (path, name, last_scanned)"
			" VALUES (?, ?, CURRENT_TIMESTAMP)");
		ins.bind(1, path_str);
		ins.bind(2, name);
		ins.exec();
		}
	else {
		SQLite::Statement ins(db_music_,
			"INSERT OR IGNORE INTO folders (path, name, parent_id, last_scanned)"
			" VALUES (?, ?, ?, CURRENT_TIMESTAMP)");
		ins.bind(1, path_str);
		ins.bind(2, name);
		ins.bind(3, parent_id);
		ins.exec();
		}

	// Also set parent_id in case the row already existed without it.
	if (parent_id < 0) {
		SQLite::Statement upd(db_music_,
			"UPDATE folders SET parent_id = NULL, last_scanned = CURRENT_TIMESTAMP"
			" WHERE path = ?");
		upd.bind(1, path_str);
		upd.exec();
		}
	else {
		SQLite::Statement upd(db_music_,
			"UPDATE folders SET parent_id = ?, last_scanned = CURRENT_TIMESTAMP"
			" WHERE path = ?");
		upd.bind(1, parent_id);
		upd.bind(2, path_str);
		upd.exec();
		}

	SQLite::Statement sel(db_music_, "SELECT id FROM folders WHERE path = ?");
	sel.bind(1, path_str);
	sel.executeStep();
	return sel.getColumn(0).getInt();
	}

int MediaStore::upsert_artist(const std::string& name)
	{
	SQLite::Statement ins(db_music_,
		"INSERT OR IGNORE INTO artists (name) VALUES (?)");
	ins.bind(1, name);
	ins.exec();

	SQLite::Statement sel(db_music_, "SELECT id FROM artists WHERE name = ?");
	sel.bind(1, name);
	sel.executeStep();
	return sel.getColumn(0).getInt();
	}

int MediaStore::upsert_album(int folder_id, const std::string& title,
                              int artist_id, int year, const std::string& genre)
	{
	{
	SQLite::Statement ins(db_music_,
		"INSERT OR IGNORE INTO albums (folder_id, title, year, genre, last_scanned)"
		" VALUES (?, ?, ?, ?, CURRENT_TIMESTAMP)");
	ins.bind(1, folder_id);
	ins.bind(2, title);
	ins.bind(3, year);
	ins.bind(4, genre);
	ins.exec();
	}

	SQLite::Statement sel(db_music_, "SELECT id FROM albums WHERE folder_id = ?");
	sel.bind(1, folder_id);
	sel.executeStep();
	int album_id = sel.getColumn(0).getInt();

	// Link album to its artist (by folder convention: the artist dir above).
	SQLite::Statement lnk(db_music_,
		"INSERT OR IGNORE INTO album_artists (album_id, artist_id, role)"
		" VALUES (?, ?, 'albumartist')");
	lnk.bind(1, album_id);
	lnk.bind(2, artist_id);
	lnk.exec();

	return album_id;
	}

// ---- User management --------------------------------------------------

bool MediaStore::add_user(const std::string& username, const std::string& password,
                           bool is_admin)
	{
	std::lock_guard<std::mutex> lock(db_mutex_);
	SQLite::Statement ins(db_music_,
		"INSERT OR IGNORE INTO client.users (username, password_enc, is_admin) VALUES (?,?,?)");
	ins.bind(1, username);
	ins.bind(2, password);
	ins.bind(3, is_admin ? 1 : 0);
	ins.exec();
	return db_music_.getChanges() > 0;
	}

bool MediaStore::has_users()
	{
	std::lock_guard<std::mutex> lock(db_mutex_);
	SQLite::Statement sel(db_music_, "SELECT COUNT(*) FROM client.users");
	sel.executeStep();
	return sel.getColumn(0).getInt() > 0;
	}

std::optional<MediaStore::UserInfo> MediaStore::get_user(const std::string& username)
	{
	std::lock_guard<std::mutex> lock(db_mutex_);
	SQLite::Statement sel(db_music_,
		"SELECT username, email, is_admin, max_bitrate, upload_allowed, disabled, cast_allowed"
		" FROM client.users WHERE username = ?");
	sel.bind(1, username);
	if (!sel.executeStep()) return std::nullopt;
	UserInfo u;
	u.username       = sel.getColumn(0).getString();
	u.email          = sel.getColumn(1).isNull() ? "" : sel.getColumn(1).getString();
	u.is_admin       = sel.getColumn(2).getInt() != 0;
	u.max_bitrate    = sel.getColumn(3).getInt();
	u.upload_allowed = sel.getColumn(4).getInt() != 0;
	u.disabled       = sel.getColumn(5).getInt() != 0;
	u.cast_allowed   = sel.getColumn(6).getInt() != 0;
	return u;
	}

std::vector<MediaStore::UserInfo> MediaStore::list_users()
	{
	std::lock_guard<std::mutex> lock(db_mutex_);
	SQLite::Statement sel(db_music_,
		"SELECT username, email, is_admin, max_bitrate, upload_allowed, disabled, cast_allowed"
		" FROM client.users ORDER BY username");
	std::vector<UserInfo> result;
	while (sel.executeStep()) {
		UserInfo u;
		u.username       = sel.getColumn(0).getString();
		u.email          = sel.getColumn(1).isNull() ? "" : sel.getColumn(1).getString();
		u.is_admin       = sel.getColumn(2).getInt() != 0;
		u.max_bitrate    = sel.getColumn(3).getInt();
		u.upload_allowed = sel.getColumn(4).getInt() != 0;
		u.disabled       = sel.getColumn(5).getInt() != 0;
		u.cast_allowed   = sel.getColumn(6).getInt() != 0;
		result.push_back(u);
		}
	return result;
	}

bool MediaStore::update_user(const std::string& username,
                              const std::string& new_password,
                              const std::string& email,
                              bool is_admin,
                              int  max_bitrate,
                              bool upload_allowed,
                              bool disabled,
                              bool cast_allowed)
	{
	std::lock_guard<std::mutex> lock(db_mutex_);
	if (new_password.empty()) {
		SQLite::Statement upd(db_music_,
			"UPDATE client.users"
			" SET email=?, is_admin=?, max_bitrate=?, upload_allowed=?, disabled=?, cast_allowed=?"
			" WHERE username=?");
		upd.bind(1, email);
		upd.bind(2, is_admin       ? 1 : 0);
		upd.bind(3, max_bitrate);
		upd.bind(4, upload_allowed ? 1 : 0);
		upd.bind(5, disabled       ? 1 : 0);
		upd.bind(6, cast_allowed   ? 1 : 0);
		upd.bind(7, username);
		upd.exec();
		}
	else {
		SQLite::Statement upd(db_music_,
			"UPDATE client.users"
			" SET password_enc=?, email=?, is_admin=?, max_bitrate=?, upload_allowed=?, disabled=?, cast_allowed=?"
			" WHERE username=?");
		upd.bind(1, new_password);
		upd.bind(2, email);
		upd.bind(3, is_admin       ? 1 : 0);
		upd.bind(4, max_bitrate);
		upd.bind(5, upload_allowed ? 1 : 0);
		upd.bind(6, disabled       ? 1 : 0);
		upd.bind(7, cast_allowed   ? 1 : 0);
		upd.bind(8, username);
		upd.exec();
		}
	return db_music_.getChanges() > 0;
	}

bool MediaStore::validate_auth(const std::string& username,
                                const std::string& password,
                                const std::string& token,
                                const std::string& salt)
	{
	std::lock_guard<std::mutex> lock(db_mutex_);
	SQLite::Statement sel(db_music_,
		"SELECT password_enc, disabled FROM client.users WHERE username = ?");
	sel.bind(1, username);
	if (!sel.executeStep()) return false;
	std::string stored   = sel.getColumn(0).getString();
	bool        disabled = sel.getColumn(1).getInt() != 0;
	if (disabled) return false;

	bool ok = false;

	if (!password.empty()) {
		// Some clients send p=enc:HEXHEX (hex-encoded plaintext password).
		std::string plain = password;
		if (plain.size() > 4 && plain.substr(0, 4) == "enc:") {
			std::string hex = plain.substr(4);
			plain.clear();
			for (size_t i = 0; i + 1 < hex.size(); i += 2)
				plain += static_cast<char>(std::stoi(hex.substr(i, 2), nullptr, 16));
			}
		ok = (plain == stored);
		}
	else if (!token.empty() && !salt.empty()) {
		std::string expected = md5_hex(stored + salt);
		std::string tok = token;
		std::transform(tok.begin(), tok.end(), tok.begin(), ::tolower);
		ok = (tok == expected);
		}

	if (ok) {
		SQLite::Statement upd(db_music_,
			"UPDATE client.users SET last_access = CURRENT_TIMESTAMP WHERE username = ?");
		upd.bind(1, username);
		upd.exec();
		}

	return ok;
	}

// ---- Library browsing ------------------------------------------------

std::string MediaStore::get_folder_name(int folder_id)
	{
	std::lock_guard<std::mutex> lock(db_mutex_);
	SQLite::Statement q(db_music_, "SELECT name FROM folders WHERE id = ?");
	q.bind(1, folder_id);
	return q.executeStep() ? q.getColumn(0).getString() : "";
	}

std::string MediaStore::get_folder_path(int folder_id)
	{
	std::lock_guard<std::mutex> lock(db_mutex_);
	SQLite::Statement q(db_music_, "SELECT path FROM folders WHERE id = ?");
	q.bind(1, folder_id);
	return q.executeStep() ? q.getColumn(0).getString() : "";
	}

std::optional<MediaStore::CachedArtistInfo> MediaStore::get_cached_artist_info(int folder_id)
	{
	std::lock_guard<std::mutex> lock(db_mutex_);
	SQLite::Statement q(db_music_,
		"SELECT mbid, last_fm_url, biography, image_url, wiki_url, allmusic_url, discogs_url"
		" FROM artist_info_cache WHERE folder_id = ?");
	q.bind(1, folder_id);
	if (!q.executeStep()) return std::nullopt;
	CachedArtistInfo a;
	a.mbid         = q.getColumn(0).getString();
	a.last_fm_url  = q.getColumn(1).getString();
	a.biography    = q.getColumn(2).getString();
	a.image_url    = q.getColumn(3).getString();
	a.wiki_url     = q.getColumn(4).getString();
	a.allmusic_url = q.getColumn(5).getString();
	a.discogs_url  = q.getColumn(6).getString();
	return a;
	}

void MediaStore::cache_artist_info(int folder_id, const CachedArtistInfo& info)
	{
	std::lock_guard<std::mutex> lock(db_mutex_);
	SQLite::Statement ins(db_music_,
		"INSERT OR REPLACE INTO artist_info_cache"
		" (folder_id, mbid, last_fm_url, biography, image_url, wiki_url, allmusic_url, discogs_url)"
		" VALUES (?, ?, ?, ?, ?, ?, ?, ?)");
	ins.bind(1, folder_id);
	ins.bind(2, info.mbid);
	ins.bind(3, info.last_fm_url);
	ins.bind(4, info.biography);
	ins.bind(5, info.image_url);
	ins.bind(6, info.wiki_url);
	ins.bind(7, info.allmusic_url);
	ins.bind(8, info.discogs_url);
	ins.exec();
	}

// One query per artist rather than one per song: the scan asks this before it
// decides which files still need art, exactly as Phase 2 fetches every known
// mtime in one go.
std::unordered_map<std::string, int64_t>
MediaStore::load_video_art_keys(const std::string& path_prefix)
	{
	std::lock_guard<std::mutex> lock(db_mutex_);
	std::unordered_map<std::string, int64_t> result;
	SQLite::Statement q(db_music_,
		"SELECT path, file_modified FROM video_art WHERE path LIKE ?");
	q.bind(1, path_prefix);
	while (q.executeStep())
		result[q.getColumn(0).getString()] = q.getColumn(1).getInt64();
	return result;
	}

void MediaStore::store_video_art(const std::string& rel_path, int64_t mtime,
                                  const std::string& mime,
                                  const std::string& source,
                                  const std::string& bytes)
	{
	std::lock_guard<std::mutex> lock(db_mutex_);
	SQLite::Statement ins(db_music_,
		"INSERT OR REPLACE INTO video_art"
		" (path, file_modified, mime, source, image)"
		" VALUES (?, ?, ?, ?, ?)");
	ins.bind(1, rel_path);
	ins.bind(2, mtime);
	ins.bind(3, mime);
	ins.bind(4, source);
	ins.bind(5, bytes.data(), static_cast<int>(bytes.size()));
	ins.exec();
	}

std::optional<MediaStore::VideoArtRow>
MediaStore::get_video_art(const std::string& rel_path)
	{
	std::lock_guard<std::mutex> lock(db_mutex_);
	SQLite::Statement q(db_music_,
		"SELECT mime, image, file_modified FROM video_art WHERE path = ?");
	q.bind(1, rel_path);
	if (!q.executeStep()) return std::nullopt;
	auto blob = q.getColumn(1);
	// A zero-length blob would hand assign() a null pointer, and would reach a
	// client as a 200 with an empty body — which renders as a broken image and
	// looks like a missing file rather than like a bad row.
	if (blob.getBytes() <= 0 || blob.getBlob() == nullptr) return std::nullopt;
	VideoArtRow r;
	r.mime = q.getColumn(0).getString();
	r.bytes.assign(static_cast<const char*>(blob.getBlob()),
	               static_cast<size_t>(blob.getBytes()));
	r.file_modified = q.getColumn(2).getInt64();
	return r;
	}

std::optional<MediaStore::VideoMetaRow>
MediaStore::get_video_meta(const std::string& rel_path)
	{
	std::lock_guard<std::mutex> lock(db_mutex_);
	SQLite::Statement q(db_music_,
		"SELECT query, media_type, tmdb_id, title, year, overview, status,"
		"       fetched_at, poster_path"
		" FROM video_meta WHERE path = ?");
	q.bind(1, rel_path);
	if (!q.executeStep()) return std::nullopt;
	VideoMetaRow r;
	r.query      = q.getColumn(0).getString();
	r.media_type = q.getColumn(1).getString();
	r.tmdb_id    = q.getColumn(2).isNull() ? 0 : q.getColumn(2).getInt();
	r.title      = q.getColumn(3).isNull() ? "" : q.getColumn(3).getString();
	r.year       = q.getColumn(4).isNull() ? 0 : q.getColumn(4).getInt();
	r.overview   = q.getColumn(5).isNull() ? "" : q.getColumn(5).getString();
	r.status      = q.getColumn(6).getString();
	r.fetched_at  = q.getColumn(7).getInt64();
	r.poster_path = q.getColumn(8).isNull() ? "" : q.getColumn(8).getString();
	return r;
	}

void MediaStore::store_video_meta(const std::string& rel_path,
                                   const VideoMetaRow& row)
	{
	std::lock_guard<std::mutex> lock(db_mutex_);
	SQLite::Statement ins(db_music_,
		"INSERT OR REPLACE INTO video_meta"
		" (path, query, media_type, tmdb_id, title, year, overview, status,"
		"  poster_path, fetched_at)"
		" VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, strftime('%s','now'))");
	ins.bind(1, rel_path);
	ins.bind(2, row.query);
	ins.bind(3, row.media_type);
	ins.bind(4, row.tmdb_id);
	ins.bind(5, row.title);
	ins.bind(6, row.year);
	ins.bind(7, row.overview);
	ins.bind(8, row.status);
	ins.bind(9, row.poster_path);
	ins.exec();
	}

std::optional<MediaStore::CachedAlbumInfo>
MediaStore::get_cached_album_info(int folder_id)
	{
	std::lock_guard<std::mutex> lock(db_mutex_);
	SQLite::Statement q(db_music_,
		"SELECT mbid, notes, wiki_url, allmusic_url FROM album_info_cache WHERE folder_id = ?");
	q.bind(1, folder_id);
	if (!q.executeStep()) return std::nullopt;
	CachedAlbumInfo a;
	a.mbid         = q.getColumn(0).getString();
	a.notes        = q.getColumn(1).getString();
	a.wiki_url     = q.getColumn(2).getString();
	a.allmusic_url = q.getColumn(3).getString();
	return a;
	}

void MediaStore::cache_album_info(int folder_id, const CachedAlbumInfo& info)
	{
	std::lock_guard<std::mutex> lock(db_mutex_);
	SQLite::Statement ins(db_music_,
		"INSERT OR REPLACE INTO album_info_cache"
		" (folder_id, mbid, notes, wiki_url, allmusic_url, fetched_at)"
		" VALUES (?, ?, ?, ?, ?, strftime('%s','now'))");
	ins.bind(1, folder_id);
	ins.bind(2, info.mbid);
	ins.bind(3, info.notes);
	ins.bind(4, info.wiki_url);
	ins.bind(5, info.allmusic_url);
	ins.exec();
	}

std::vector<MediaStore::MusicFolder> MediaStore::get_music_folders()
	{
	std::lock_guard<std::mutex> lock(db_mutex_);
	// Name is the root's configured name (which is also its stored path), not
	// the directory basename: the basename can change without the library
	// changing, and clients use this to label a folder they may have pinned.
	SQLite::Statement q(db_music_,
		"SELECT id, path, COALESCE(content_type, 'artists')"
		" FROM folders WHERE parent_id IS NULL ORDER BY path");
	std::vector<MusicFolder> result;
	while (q.executeStep()) {
		std::string name = q.getColumn(1).getString();
		// The uploads root is per-user personal space, not a shared library.
		// It gets a root row because scan_dirs() runs over uploaded batches,
		// but it must never be offered as something to browse.
		if (!uploads_prefix_.empty() && name + "/" == uploads_prefix_) continue;
		result.push_back({ q.getColumn(0).getInt(), name,
		                   q.getColumn(2).getString() });
		}
	return result;
	}

std::string MediaStore::get_cover_path(int cover_art_id)
	{
	std::lock_guard<std::mutex> lock(db_mutex_);
	if (cover_art_id >= SONG_COVER_ID_BASE) {
		SQLite::Statement q(db_music_,
			"SELECT cover_path FROM songs WHERE id = ?");
		q.bind(1, cover_art_id - SONG_COVER_ID_BASE);
		if (!q.executeStep() || q.getColumn(0).isNull()) return "";
		return q.getColumn(0).getString();
		}
	SQLite::Statement q(db_music_,
		"SELECT cover_path FROM albums WHERE folder_id = ?");
	q.bind(1, cover_art_id);
	if (!q.executeStep() || q.getColumn(0).isNull()) return "";
	return q.getColumn(0).getString();
	}


std::vector<std::string> MediaStore::get_extra_image_paths(int folder_id)
	{
	std::string cover  = get_cover_path(folder_id);
	// Empty for a song's sidecar cover id, which has no folder and no extras.
	std::string folder = get_folder_path(folder_id);
	if (cover.empty() || folder.empty()) return {};

	// A folder with subfolders is a section that also holds loose files: the
	// subfolders are its albums, and their images are not this one's extras.
	bool recurse;
	{
	std::lock_guard<std::mutex> lock(db_mutex_);
	SQLite::Statement q(db_music_,
		"SELECT 1 FROM folders WHERE parent_id = ? LIMIT 1");
	q.bind(1, folder_id);
	recurse = !q.executeStep();
	}

	// find_extra_images works in absolute paths (it walks the filesystem);
	// we strip back to relative on return so the public API stays uniform.
	auto abs_results = find_extra_images(abs_path(folder), abs_path(cover), recurse);
	std::vector<std::string> result;
	result.reserve(abs_results.size());
	for (auto& p : abs_results)
		result.push_back(strip_root(p));
	return result;
	}

int MediaStore::get_image_count(int folder_id)
	{
	if (get_cover_path(folder_id).empty()) return 0;
	return 1 + static_cast<int>(get_extra_image_paths(folder_id).size());
	}

std::optional<MediaStore::SongInfo> MediaStore::get_song(int song_id)
	{
	std::lock_guard<std::mutex> lock(db_mutex_);
	SQLite::Statement q(db_music_,
		"SELECT id, path, codec, bitrate, duration, file_size, file_modified,"
		"       is_video, width, height, video_codec, audio_codec"
		" FROM songs WHERE id = ?");
	q.bind(1, song_id);
	if (!q.executeStep()) return std::nullopt;
	SongInfo s;
	s.id            = q.getColumn(0).getInt();
	s.path          = q.getColumn(1).getString();
	s.codec         = q.getColumn(2).isNull() ? "" : q.getColumn(2).getString();
	s.bitrate       = q.getColumn(3).isNull() ? 0  : q.getColumn(3).getInt();
	s.duration      = q.getColumn(4).getDouble();
	s.file_size     = q.getColumn(5).isNull() ? 0  : q.getColumn(5).getInt64();
	s.file_modified = q.getColumn(6).isNull() ? 0  : q.getColumn(6).getInt64();
	s.is_video      = q.getColumn(7).getInt() != 0;
	s.width         = q.getColumn(8).isNull()  ? 0  : q.getColumn(8).getInt();
	s.height        = q.getColumn(9).isNull()  ? 0  : q.getColumn(9).getInt();
	s.video_codec   = q.getColumn(10).isNull() ? "" : q.getColumn(10).getString();
	s.audio_codec   = q.getColumn(11).isNull() ? "" : q.getColumn(11).getString();
	return s;
	}

std::vector<MediaStore::ChildEntry> MediaStore::get_videos()
	{
	std::lock_guard<std::mutex> lock(db_mutex_);
	// Same shape as the other ChildEntry queries, with the two video columns
	// appended.  COALESCE on the album folder for parent and cover art for the
	// same reason documented in ISSUES.md: a song's own folder_id is the disc
	// (or season) subdirectory, which has no albums row to resolve a cover on.
	SQLite::Statement q(db_music_,
		"SELECT s.id, s.title, s.track_number, s.disc_number,"
		"       s.year, s.genre, s.duration, s.bitrate,"
		"       s.file_size, s.codec, COALESCE(al.folder_id, s.folder_id),"
		"       COALESCE(a.name,'') AS artist,"
		"       COALESCE(al.title,'') AS album,"
		+ SONG_COVER_ART_SQL +
		"       s.path, s.width, s.height, s.video_codec, s.audio_codec"
		" FROM songs s"
		" LEFT JOIN albums al ON al.id = s.album_id"
		" LEFT JOIN song_artists sa ON sa.song_id = s.id AND sa.role = 'artist'"
		" LEFT JOIN artists a ON a.id = sa.artist_id"
		" WHERE s.is_video = 1" + not_uploads("s.path") +
		" ORDER BY artist COLLATE NOCASE, album COLLATE NOCASE,"
		"          s.disc_number, s.track_number, s.title COLLATE NOCASE");
	std::vector<ChildEntry> result;
	while (q.executeStep()) {
		ChildEntry e;
		e.id           = q.getColumn(0).getInt();
		e.is_dir       = false;
		e.title        = q.getColumn(1).getString();
		e.track_number = q.getColumn(2).getInt();
		e.disc_number  = q.getColumn(3).getInt();
		e.year         = q.getColumn(4).getInt();
		e.genre        = q.getColumn(5).isNull() ? "" : q.getColumn(5).getString();
		e.duration     = q.getColumn(6).getDouble();
		e.bitrate      = q.getColumn(7).getInt();
		e.file_size    = q.getColumn(8).getInt64();
		e.codec        = q.getColumn(9).isNull() ? "" : q.getColumn(9).getString();
		e.parent_id    = q.getColumn(10).getInt();
		e.artist       = q.getColumn(11).getString();
		e.album        = q.getColumn(12).getString();
		e.cover_art_id = q.getColumn(13).getInt();
		e.path         = q.getColumn(14).getString();
		e.width        = q.getColumn(15).isNull() ? 0 : q.getColumn(15).getInt();
		e.height       = q.getColumn(16).isNull() ? 0 : q.getColumn(16).getInt();
		e.video_codec  = q.getColumn(17).isNull() ? "" : q.getColumn(17).getString();
		e.audio_codec  = q.getColumn(18).isNull() ? "" : q.getColumn(18).getString();
		result.push_back(std::move(e));
		}
	return result;
	}

std::string MediaStore::get_captions_vtt(int song_id, int stream_index)
	{
	auto song = get_song(song_id);
	if (!song || !song->is_video) return {};

	std::string abs = abs_path(song->path);
	if (!path_is_within_root(abs)) return {};

	std::string source = abs;
	if (stream_index < 0) {
		// Sidecar, in preference order.  A .vtt still goes through ffmpeg so
		// the response is normalised (and so a mislabelled file cannot be
		// served verbatim).
		fs::path base = fs::path(abs);
		bool     found = false;
		for (const char* ext : { ".vtt", ".srt", ".ass", ".ssa" }) {
			fs::path cand = base;
			cand.replace_extension(ext);
			std::error_code fec;
			if (fs::exists(cand, fec) && path_is_within_root(cand)) {
				source = cand.string();
				found  = true;
				break;
				}
			}
		if (!found) return {};
		}

	std::vector<std::string> args = { "ffmpeg", "-v", "quiet", "-i", source };
	if (stream_index >= 0) {
		args.push_back("-map");
		args.push_back("0:" + std::to_string(stream_index));
		}
	args.push_back("-f");
	args.push_back("webvtt");
	args.push_back("pipe:1");

	reproc::process proc;
	reproc::options opts;
	opts.redirect.err.type = reproc::redirect::type::discard;
	if (proc.start(args, opts)) return {};

	std::string          out;
	reproc::sink::string sink(out);
	auto ec            = reproc::drain(proc, sink, reproc::sink::null);
	auto [status, wec] = proc.wait(reproc::infinite);
	if (ec || wec || status != 0) return {};
	return out;
	}

MediaStore::VideoStreams MediaStore::get_video_streams(int song_id)
	{
	VideoStreams vs;
	auto song = get_song(song_id);   // takes db_mutex_ itself; don't hold it here
	if (!song || !song->is_video) return vs;

	std::string abs = abs_path(song->path);
	if (!path_is_within_root(abs)) return vs;

	std::vector<std::string> args = {
		"ffprobe", "-v", "quiet", "-print_format", "json", "-show_streams", abs
		};
	reproc::process proc;
	reproc::options opts;
	opts.redirect.err.type = reproc::redirect::type::discard;
	if (proc.start(args, opts)) return vs;

	std::string          out;
	reproc::sink::string sink(out);
	auto ec            = reproc::drain(proc, sink, reproc::sink::null);
	auto [status, wec] = proc.wait(reproc::infinite);
	if (ec || wec || status != 0) return vs;

	try {
		auto j       = nlohmann::json::parse(out);
		auto streams = j.find("streams");
		if (streams == j.end() || !streams->is_array()) return vs;
		for (const auto& s : *streams) {
			auto type = s.value("codec_type", std::string());
			if (type != "subtitle" && type != "audio") continue;
			// DVD and Blu-ray subtitles are bitmaps, not text: there is no
			// WebVTT to convert them into, and ffmpeg fails outright if asked.
			// Offering them would mean every DVD's caption list is a list of
			// tracks that 500 when selected.
			auto codec = s.value("codec_name", std::string());
			if (type == "subtitle"
			        && (codec == "dvd_subtitle" || codec == "hdmv_pgs_subtitle"
			            || codec == "dvb_subtitle" || codec == "xsub"))
				continue;
			CaptionTrack t;
			t.index = static_cast<int>(probe_num(s, "index"));
			if (auto tags = s.find("tags"); tags != s.end()) {
				t.language = tags->value("language", std::string());
				t.title    = tags->value("title",    std::string());
				}
			if (t.title.empty()) t.title = s.value("codec_name", std::string());
			(type == "subtitle" ? vs.captions : vs.audio_tracks)
				.push_back(std::move(t));
			}
		}
	catch (const std::exception&) {}
	return vs;
	}

std::vector<MediaStore::ArtistDir> MediaStore::get_artist_dirs(
	const std::string& personal_user, int music_folder_id,
	const std::string& content_type)
	{
	std::lock_guard<std::mutex> lock(db_mutex_);
	// Count albums per artist via the child folders (album folders are one level
	// down), plus an album on the folder itself for loose files sitting beside
	// them.
	// "IN (…roots…)" rather than "= root": level-1 entries now come from every
	// configured root, and which root a folder belongs to is visible in its
	// path prefix rather than in a column.
	// A root row joins the list only when it carries loose files of its own.
	// Its name comes from `path`, not `name`: for a root those differ, and
	// `name` is the directory basename, which is never exposed.
	std::string sql =
		"SELECT f.id,"
		"       CASE WHEN f.parent_id IS NULL THEN f.path ELSE f.name END AS name,"
		"       COUNT(al.id) + (SELECT COUNT(*) FROM albums own"
		"                        WHERE own.folder_id = f.id) AS album_count"
		" FROM folders f"
		" LEFT JOIN folders af ON af.parent_id = f.id"
		" LEFT JOIN albums al ON al.folder_id = af.id"
		" WHERE (f.parent_id IN (SELECT id FROM folders WHERE parent_id IS NULL";
	// Narrow the *root set* by kind rather than the folders themselves: one
	// kind can span several roots, so this must not become an equality on a
	// single parent id.  COALESCE because a root row written before
	// content_type existed defaults to artists.
	if (!content_type.empty())
		sql += " AND COALESCE(content_type, 'artists') = ?";
	sql += ")";
	sql += " OR (f.parent_id IS NULL";
	if (!content_type.empty())
		sql += " AND COALESCE(f.content_type, 'artists') = ?";
	sql += "     AND EXISTS (SELECT 1 FROM albums own WHERE own.folder_id = f.id)))";
	if (music_folder_id > 0)
		sql += " AND (f.parent_id = ? OR f.id = ?)";
	if (personal_user.empty())
		sql += not_uploads("f.path");
	else
		sql += " AND f.path LIKE ?";
	sql += " GROUP BY f.id ORDER BY name COLLATE NOCASE";

	SQLite::Statement sel(db_music_, sql);
	int idx = 1;
	// Bind order follows the order the fragments were appended above.
	if (!content_type.empty())    sel.bind(idx++, content_type);
	if (!content_type.empty())    sel.bind(idx++, content_type);
	if (music_folder_id > 0)      sel.bind(idx++, music_folder_id);
	if (music_folder_id > 0)      sel.bind(idx++, music_folder_id);
	if (!personal_user.empty())
		sel.bind(idx++, uploads_prefix_ + personal_user + "/%");

	std::vector<ArtistDir> result;
	while (sel.executeStep())
		result.push_back({sel.getColumn(0).getInt(),
		                  sel.getColumn(1).getString(),
		                  sel.getColumn(2).getInt()});
	return result;
	}

std::optional<MediaStore::DirInfo> MediaStore::get_directory(int folder_id,
                                                               bool flat_multi_disc)
	{
	std::lock_guard<std::mutex> lock(db_mutex_);

	// Fetch the folder itself. al.id tells us whether this is an album folder.
	// A root is named by its `path`; its `name` is the directory basename,
	// which is never exposed.
	SQLite::Statement fsel(db_music_,
		"SELECT f.id,"
		"       CASE WHEN f.parent_id IS NULL THEN f.path ELSE f.name END,"
		"       f.parent_id,"
		"       al.id AS album_id,"
		"       CASE WHEN al.cover_path IS NOT NULL AND al.cover_path != ''"
		"            THEN f.id ELSE -1 END AS cover_art_id"
		" FROM folders f"
		" LEFT JOIN albums al ON al.folder_id = f.id"
		" WHERE f.id = ?");
	fsel.bind(1, folder_id);
	if (!fsel.executeStep()) return std::nullopt;

	DirInfo dir;
	dir.id           = fsel.getColumn(0).getInt();
	dir.name         = fsel.getColumn(1).getString();
	dir.parent_id    = fsel.getColumn(2).isNull() ? -1 : fsel.getColumn(2).getInt();
	bool is_album    = !fsel.getColumn(3).isNull();
	dir.cover_art_id = fsel.getColumn(4).getInt();

	// Flattening pulls in the songs of every child folder, which is right for
	// an album's disc subdirectories and wrong for a section that holds loose
	// files *and* albums: it would absorb the whole section into one listing
	// and hide the albums themselves.  A disc folder has a folders row but no
	// albums row, which is exactly the distinction needed.
	bool has_child_album = false;
	if (flat_multi_disc && is_album) {
		SQLite::Statement q(db_music_,
			"SELECT 1 FROM folders f JOIN albums al ON al.folder_id = f.id"
			" WHERE f.parent_id = ? LIMIT 1");
		q.bind(1, folder_id);
		has_child_album = q.executeStep();
		}
	bool flatten = flat_multi_disc && is_album && !has_child_album;

	// Child directories — skipped in flat mode for album folders (disc subdirs
	// are absorbed into the song list below).
	if (!flatten) {
		SQLite::Statement dsel(db_music_,
			"SELECT f.id, COALESCE(al.title, f.name) AS title,"
			"       COALESCE(a.name, '') AS artist,"
			"       COALESCE(al.title, f.name) AS album,"
			"       CASE WHEN al.cover_path IS NOT NULL AND al.cover_path != ''"
			"            THEN f.id ELSE -1 END AS cover_art_id,"
			"       COALESCE(al.year, 0) AS year"
			" FROM folders f"
			" LEFT JOIN albums al ON al.folder_id = f.id"
			" LEFT JOIN album_artists aa ON aa.album_id = al.id AND aa.role = 'albumartist'"
			" LEFT JOIN artists a ON a.id = aa.artist_id"
			" WHERE f.parent_id = ?"
			" ORDER BY f.name COLLATE NOCASE");
		dsel.bind(1, folder_id);
		while (dsel.executeStep()) {
			ChildEntry e;
			e.id           = dsel.getColumn(0).getInt();
			e.parent_id    = folder_id;
			e.is_dir       = true;
			e.title        = dsel.getColumn(1).getString();
			e.artist       = dsel.getColumn(2).getString();
			e.album        = dsel.getColumn(3).getString();
			e.cover_art_id = dsel.getColumn(4).getInt();
			e.year         = dsel.getColumn(5).getInt();
			dir.children.push_back(std::move(e));
			}
		}

	// Child songs. In flat mode for album folders, also include songs from disc
	// subfolders (one level down), ordered by disc then track. Those songs report
	// the album folder as parent, not the disc subfolder they physically sit in —
	// the listing claims to be the album's, and parent is emitted as albumId.
	const char* song_sql_flat =
		"SELECT s.id, s.title, s.track_number, s.disc_number,"
		"       s.year, s.genre, s.duration, s.bitrate,"
		"       s.file_size, s.codec, COALESCE(al.folder_id, s.folder_id),"
		"       COALESCE(a.name, '') AS artist,"
		"       COALESCE(al.title, '') AS album, s.width, s.height,"
		"       s.video_codec, s.audio_codec, s.cover_path"
		" FROM songs s"
		" LEFT JOIN albums al ON al.id = s.album_id"
		" LEFT JOIN song_artists sa ON sa.song_id = s.id AND sa.role = 'artist'"
		" LEFT JOIN artists a ON a.id = sa.artist_id"
		" WHERE s.folder_id = ?"
		"    OR s.folder_id IN (SELECT id FROM folders WHERE parent_id = ?)"
		" ORDER BY s.disc_number, s.track_number, s.filename";

	const char* song_sql_normal =
		"SELECT s.id, s.title, s.track_number, s.disc_number,"
		"       s.year, s.genre, s.duration, s.bitrate,"
		"       s.file_size, s.codec, s.folder_id,"
		"       COALESCE(a.name, '') AS artist,"
		"       COALESCE(al.title, '') AS album, s.width, s.height,"
		"       s.video_codec, s.audio_codec, s.cover_path"
		" FROM songs s"
		" LEFT JOIN albums al ON al.id = s.album_id"
		" LEFT JOIN song_artists sa ON sa.song_id = s.id AND sa.role = 'artist'"
		" LEFT JOIN artists a ON a.id = sa.artist_id"
		" WHERE s.folder_id = ?"
		" ORDER BY s.disc_number, s.track_number, s.filename";

	SQLite::Statement ssel(db_music_, flatten ? song_sql_flat : song_sql_normal);
	ssel.bind(1, folder_id);
	if (flatten) ssel.bind(2, folder_id);

	while (ssel.executeStep()) {
		ChildEntry e;
		e.id           = ssel.getColumn(0).getInt();
		e.is_dir       = false;
		e.title        = ssel.getColumn(1).getString();
		e.track_number = ssel.getColumn(2).getInt();
		e.disc_number  = ssel.getColumn(3).getInt();
		e.year         = ssel.getColumn(4).getInt();
		e.genre        = ssel.getColumn(5).isNull() ? "" : ssel.getColumn(5).getString();
		e.duration     = ssel.getColumn(6).getDouble();
		e.bitrate      = ssel.getColumn(7).getInt();
		e.file_size    = ssel.getColumn(8).getInt64();
		e.codec        = ssel.getColumn(9).isNull() ? "" : ssel.getColumn(9).getString();
		e.parent_id    = ssel.getColumn(10).getInt();  // album folder in flat mode
		e.artist       = ssel.getColumn(11).getString();
		e.album        = ssel.getColumn(12).getString();
		e.width        = ssel.getColumn(13).isNull() ? 0 : ssel.getColumn(13).getInt();
		e.height       = ssel.getColumn(14).isNull() ? 0 : ssel.getColumn(14).getInt();
		e.video_codec  = ssel.getColumn(15).isNull() ? "" : ssel.getColumn(15).getString();
		e.audio_codec  = ssel.getColumn(16).isNull() ? "" : ssel.getColumn(16).getString();
		// Songs inherit cover art from their parent album folder, unless they
		// have a sidecar image of their own — a loose file's folder cover
		// belongs to the whole section it sits in.
		if (!ssel.getColumn(17).isNull() && ssel.getColumn(17).getString() != "")
			e.cover_art_id = SONG_COVER_ID_BASE + e.id;
		else if (dir.cover_art_id >= 0)
			e.cover_art_id = dir.cover_art_id;
		dir.children.push_back(std::move(e));
		}

	return dir;
	}

// ---- Album list ----------------------------------------------------------

std::vector<MediaStore::AlbumEntry> MediaStore::get_album_list(
	const std::string& type,
	int size, int offset,
	int from_year, int to_year,
	const std::string& genre,
	const std::string& username,
	const std::string& personal_user)
	{
	std::lock_guard<std::mutex> lock(db_mutex_);

	// Base SELECT — common to all types. The trailing column is a per-user
	// album-star flag, populated from a LEFT JOIN on client.stars.
	std::string sql =
		"SELECT f.id,"
		+ ALBUM_ARTIST_ID_SQL +
		"       COALESCE(al.title, f.name),"
		"       COALESCE(a.name,''),"
		"       CASE WHEN al.cover_path IS NOT NULL AND al.cover_path != ''"
		"            THEN f.id ELSE -1 END,"
		"       COALESCE(al.song_count,0),"
		"       CAST(COALESCE(al.duration,0) AS INTEGER),"
		"       COALESCE(al.year,0),"
		"       COALESCE(al.genre,''),"
		"       COALESCE(al.created,''),"
		"       COALESCE(sa.created, '') AS starred"
		" FROM albums al"
		" JOIN folders f ON f.id = al.folder_id"
		" LEFT JOIN album_artists aa ON aa.album_id = al.id AND aa.role = 'albumartist'"
		" LEFT JOIN artists a ON a.id = aa.artist_id"
		" LEFT JOIN client.stars sa ON sa.album_folder_path = f.path"
		"      AND sa.user_id = (SELECT id FROM client.users WHERE username = ?)";

	// Extra joins for play-count-based types.
	bool play_count_join = (type == "frequent" || type == "recent");
	if (play_count_join)
		sql += " LEFT JOIN songs s ON s.album_id = al.id"
		       " LEFT JOIN client.play_counts pc ON pc.song_path = s.path"
		       " AND pc.user_id = (SELECT id FROM client.users WHERE username = ?)";

	// WHERE clause.
	bool has_where = false;
	auto add_where = [&](const std::string& clause) {
		sql += has_where ? " AND " : " WHERE ";
		sql += clause;
		has_where = true;
		};
	if      (type == "byYear")  add_where("al.year BETWEEN ? AND ?");
	else if (type == "byGenre") add_where("LOWER(COALESCE(al.genre,'')) = LOWER(?)");
	else if (type == "starred") add_where("sa.album_folder_path IS NOT NULL");

	// Braces are load-bearing: without them the else binds to the inner if,
	// and a personal listing would silently become an unfiltered one.
	if (personal_user.empty()) {
		if (!uploads_like_.empty())
			add_where("f.path NOT LIKE '" + uploads_like_ + "'");
		}
	else
		add_where("f.path LIKE ?");

	// GROUP BY needed when aggregating play counts.
	if (play_count_join) sql += " GROUP BY al.id";

	// ORDER BY.
	if      (type == "newest")              sql += " ORDER BY al.created DESC";
	else if (type == "random")              sql += " ORDER BY RANDOM()";
	else if (type == "alphabeticalByName")  sql += " ORDER BY al.title COLLATE NOCASE";
	else if (type == "alphabeticalByArtist")
		sql += " ORDER BY COALESCE(a.name,'') COLLATE NOCASE, al.title COLLATE NOCASE";
	else if (type == "frequent")            sql += " ORDER BY SUM(COALESCE(pc.count,0)) DESC";
	else if (type == "recent")              sql += " ORDER BY MAX(COALESCE(pc.last_played,'')) DESC";
	else if (type == "starred")             sql += " ORDER BY sa.created DESC";
	else if (type == "byYear")              sql += " ORDER BY al.year";
	else if (type == "byGenre")             sql += " ORDER BY al.title COLLATE NOCASE";
	else                                    sql += " ORDER BY al.created DESC"; // fallback

	sql += " LIMIT ? OFFSET ?";

	SQLite::Statement q(db_music_, sql);
	int idx = 1;

	q.bind(idx++, username);                                // album-star LEFT JOIN
	if (play_count_join)
		q.bind(idx++, username);                            // play-count LEFT JOIN
	if (type == "byYear") { q.bind(idx++, from_year); q.bind(idx++, to_year); }
	if (type == "byGenre")  q.bind(idx++, genre);
	if (!personal_user.empty())
		q.bind(idx++, uploads_prefix_ + personal_user + "/%");
	q.bind(idx++, size);
	q.bind(idx++, offset);

	std::vector<AlbumEntry> result;
	while (q.executeStep()) {
		AlbumEntry e;
		e.id           = q.getColumn(0).getInt();
		e.parent_id    = q.getColumn(1).getInt();
		e.title        = q.getColumn(2).getString();
		e.artist       = q.getColumn(3).getString();
		e.cover_art_id = q.getColumn(4).getInt();
		e.song_count   = q.getColumn(5).getInt();
		e.duration     = q.getColumn(6).getInt();
		e.year         = q.getColumn(7).getInt();
		e.genre        = q.getColumn(8).isNull() ? "" : q.getColumn(8).getString();
		e.created      = q.getColumn(9).isNull() ? "" : q.getColumn(9).getString();
		e.starred      = q.getColumn(10).getString();
		result.push_back(std::move(e));
		}
	return result;
	}

// ---- Recent songs -----------------------------------------------------------

std::vector<MediaStore::RecentSongEntry> MediaStore::get_recent_songs(
	const std::string& username,
	int size,
	int offset)
	{
	std::lock_guard<std::mutex> lock(db_mutex_);

	SQLite::Statement q(db_music_,
		"SELECT s.id, s.title, s.track_number, s.disc_number,"
		"       s.year, s.genre, s.duration, s.bitrate, s.file_size, s.codec,"
		"       al.folder_id,"
		"       COALESCE(a.name,'') AS artist,"
		"       COALESCE(al.title, f.name) AS album,"
		"       CASE WHEN al.cover_path IS NOT NULL AND al.cover_path != ''"
		"            THEN al.folder_id ELSE -1 END AS cover_art_id,"
		"       f.parent_id,"
		"       pc.last_played"
		" FROM client.play_counts pc"
		" JOIN client.users u ON u.id = pc.user_id"
		" JOIN songs s ON s.path = pc.song_path"
		" JOIN albums al ON al.id = s.album_id"
		" JOIN folders f ON f.id = al.folder_id"
		" LEFT JOIN song_artists sas ON sas.song_id = s.id AND sas.role = 'artist'"
		" LEFT JOIN artists a ON a.id = sas.artist_id"
		" WHERE u.username = ?"
		" ORDER BY pc.last_played DESC"
		" LIMIT ? OFFSET ?");
	q.bind(1, username);
	q.bind(2, size);
	q.bind(3, offset);

	std::vector<RecentSongEntry> result;
	while (q.executeStep()) {
		RecentSongEntry e;
		e.song.id           = q.getColumn(0).getInt();
		e.song.is_dir       = false;
		e.song.title        = q.getColumn(1).getString();
		e.song.track_number = q.getColumn(2).getInt();
		e.song.disc_number  = q.getColumn(3).getInt();
		e.song.year         = q.getColumn(4).getInt();
		e.song.genre        = q.getColumn(5).isNull() ? "" : q.getColumn(5).getString();
		e.song.duration     = q.getColumn(6).getDouble();
		e.song.bitrate      = q.getColumn(7).getInt();
		e.song.file_size    = q.getColumn(8).getInt64();
		e.song.codec        = q.getColumn(9).isNull() ? "" : q.getColumn(9).getString();
		e.song.parent_id    = q.getColumn(10).getInt();
		e.song.artist       = q.getColumn(11).getString();
		e.song.album        = q.getColumn(12).getString();
		e.song.cover_art_id = q.getColumn(13).getInt();
		// column 14 (f.parent_id) unused — parent_id is already the album folder
		e.last_played       = q.getColumn(15).isNull() ? "" : q.getColumn(15).getString();
		result.push_back(std::move(e));
		}
	return result;
	}

std::optional<MediaStore::ArtistInfo> MediaStore::get_artist(int folder_id,
                                                              const std::string& username)
	{
	std::lock_guard<std::mutex> lock(db_mutex_);

	// Look up the artist folder itself.  A root reached this way (it holds
	// loose files, so it is its own artist) is named by its `path`: a root's
	// `name` is the directory basename, which is never exposed.
	SQLite::Statement fsel(db_music_,
		"SELECT id, CASE WHEN parent_id IS NULL THEN path ELSE name END"
		" FROM folders WHERE id = ?");
	fsel.bind(1, folder_id);
	if (!fsel.executeStep()) return std::nullopt;

	ArtistInfo info;
	info.artist.id   = fsel.getColumn(0).getInt();
	info.artist.name = fsel.getColumn(1).getString();

	// Fetch albums whose folder is a direct child of this artist folder, plus
	// an album on the folder itself — the loose files that sit beside the
	// albums rather than inside one.
	// Trailing column is the per-user album-star flag.
	SQLite::Statement asel(db_music_,
		"SELECT f.id,"
		+ ALBUM_ARTIST_ID_SQL +
		"       COALESCE(al.title, f.name),"
		"       COALESCE(a.name,''),"
		"       CASE WHEN al.cover_path IS NOT NULL AND al.cover_path != ''"
		"            THEN f.id ELSE -1 END,"
		"       COALESCE(al.song_count,0),"
		"       CAST(COALESCE(al.duration,0) AS INTEGER),"
		"       COALESCE(al.year,0),"
		"       COALESCE(al.genre,''),"
		"       COALESCE(al.created,''),"
		"       COALESCE(sa.created, '') AS starred"
		" FROM albums al"
		" JOIN folders f ON f.id = al.folder_id"
		" LEFT JOIN album_artists aa ON aa.album_id = al.id AND aa.role = 'albumartist'"
		" LEFT JOIN artists a ON a.id = aa.artist_id"
		" LEFT JOIN client.stars sa ON sa.album_folder_path = f.path"
		"      AND sa.user_id = (SELECT id FROM client.users WHERE username = ?)"
		" WHERE f.parent_id = ? OR f.id = ?"
		" ORDER BY al.year, al.title COLLATE NOCASE");
	asel.bind(1, username);
	asel.bind(2, folder_id);
	asel.bind(3, folder_id);

	while (asel.executeStep()) {
		AlbumEntry e;
		e.id           = asel.getColumn(0).getInt();
		e.parent_id    = asel.getColumn(1).getInt();
		e.title        = asel.getColumn(2).getString();
		e.artist       = asel.getColumn(3).getString();
		e.cover_art_id = asel.getColumn(4).getInt();
		e.song_count   = asel.getColumn(5).getInt();
		e.duration     = asel.getColumn(6).getInt();
		e.year         = asel.getColumn(7).getInt();
		e.genre        = asel.getColumn(8).isNull() ? "" : asel.getColumn(8).getString();
		e.created      = asel.getColumn(9).isNull() ? "" : asel.getColumn(9).getString();
		e.starred      = asel.getColumn(10).getString();
		info.albums.push_back(std::move(e));
		}

	info.artist.album_count = (int)info.albums.size();
	return info;
	}

std::optional<MediaStore::AlbumInfo> MediaStore::get_album(int folder_id,
                                                             bool flat_multi_disc,
                                                             const std::string& username)
	{
	std::lock_guard<std::mutex> lock(db_mutex_);

	// Fetch album metadata. Trailing column is the per-user album-star flag.
	SQLite::Statement msel(db_music_,
		"SELECT f.id,"
		+ ALBUM_ARTIST_ID_SQL +
		"       COALESCE(al.title, f.name),"
		"       COALESCE(a.name,''),"
		"       CASE WHEN al.cover_path IS NOT NULL AND al.cover_path != ''"
		"            THEN f.id ELSE -1 END,"
		"       COALESCE(al.song_count,0),"
		"       CAST(COALESCE(al.duration,0) AS INTEGER),"
		"       COALESCE(al.year,0),"
		"       COALESCE(al.genre,''),"
		"       COALESCE(al.created,''),"
		"       COALESCE(sa.created, '') AS starred"
		" FROM albums al"
		" JOIN folders f ON f.id = al.folder_id"
		" LEFT JOIN album_artists aa ON aa.album_id = al.id AND aa.role = 'albumartist'"
		" LEFT JOIN artists a ON a.id = aa.artist_id"
		" LEFT JOIN client.stars sa ON sa.album_folder_path = f.path"
		"      AND sa.user_id = (SELECT id FROM client.users WHERE username = ?)"
		" WHERE f.id = ?");
	msel.bind(1, username);
	msel.bind(2, folder_id);
	if (!msel.executeStep()) return std::nullopt;

	AlbumInfo info;
	info.album.id           = msel.getColumn(0).getInt();
	info.album.parent_id    = msel.getColumn(1).getInt();
	info.album.title        = msel.getColumn(2).getString();
	info.album.artist       = msel.getColumn(3).getString();
	info.album.cover_art_id = msel.getColumn(4).getInt();
	info.album.song_count   = msel.getColumn(5).getInt();
	info.album.duration     = msel.getColumn(6).getInt();
	info.album.year         = msel.getColumn(7).getInt();
	info.album.genre        = msel.getColumn(8).isNull() ? "" : msel.getColumn(8).getString();
	info.album.created      = msel.getColumn(9).isNull() ? "" : msel.getColumn(9).getString();
	info.album.starred      = msel.getColumn(10).getString();

	// Fetch songs, flattening disc subfolders when flat_multi_disc is set.
	// The starred LEFT JOIN is added only when a username is provided.
	const std::string star_col  = username.empty()
		? ", '' AS starred"
		: ", COALESCE(st.created, '') AS starred";
	const std::string star_join = username.empty()
		? ""
		: " LEFT JOIN client.stars st ON st.song_path = s.path"
		  " AND st.user_id = (SELECT id FROM client.users WHERE username = ?)";

	// Flat mode absorbs disc-subfolder songs into the album listing, so they report
	// the album folder as parent rather than the disc folder (see get_directory).
	std::string song_sql_flat =
		"SELECT s.id, s.title, s.track_number, s.disc_number,"
		"       s.year, s.genre, s.duration, s.bitrate,"
		"       s.file_size, s.codec, COALESCE(al.folder_id, s.folder_id),"
		"       COALESCE(a.name, '') AS artist,"
		"       COALESCE(al.title, '') AS album"
		+ star_col +
		", s.width, s.height, s.video_codec, s.audio_codec, s.cover_path"
		" FROM songs s"
		" LEFT JOIN albums al ON al.id = s.album_id"
		" LEFT JOIN song_artists sa ON sa.song_id = s.id AND sa.role = 'artist'"
		" LEFT JOIN artists a ON a.id = sa.artist_id"
		+ star_join +
		" WHERE s.folder_id = ?"
		"    OR s.folder_id IN (SELECT id FROM folders WHERE parent_id = ?)"
		" ORDER BY s.disc_number, s.track_number, s.filename";

	std::string song_sql_normal =
		"SELECT s.id, s.title, s.track_number, s.disc_number,"
		"       s.year, s.genre, s.duration, s.bitrate,"
		"       s.file_size, s.codec, s.folder_id,"
		"       COALESCE(a.name, '') AS artist,"
		"       COALESCE(al.title, '') AS album"
		+ star_col +
		", s.width, s.height, s.video_codec, s.audio_codec, s.cover_path"
		" FROM songs s"
		" LEFT JOIN albums al ON al.id = s.album_id"
		" LEFT JOIN song_artists sa ON sa.song_id = s.id AND sa.role = 'artist'"
		" LEFT JOIN artists a ON a.id = sa.artist_id"
		+ star_join +
		" WHERE s.folder_id = ?"
		" ORDER BY s.disc_number, s.track_number, s.filename";

	// As in get_directory(): flattening absorbs the songs of every child
	// folder, and for a loose-file album those children are the section's
	// other albums, not disc subdirectories.
	bool has_child_album = false;
	if (flat_multi_disc) {
		SQLite::Statement q(db_music_,
			"SELECT 1 FROM folders f JOIN albums al ON al.folder_id = f.id"
			" WHERE f.parent_id = ? LIMIT 1");
		q.bind(1, folder_id);
		has_child_album = q.executeStep();
		}
	bool flatten = flat_multi_disc && !has_child_album;

	SQLite::Statement ssel(db_music_, flatten ? song_sql_flat : song_sql_normal);
	int idx = 1;
	if (!username.empty())
		ssel.bind(idx++, username);
	ssel.bind(idx++, folder_id);
	if (flatten) ssel.bind(idx, folder_id);

	while (ssel.executeStep()) {
		ChildEntry e;
		e.id           = ssel.getColumn(0).getInt();
		e.is_dir       = false;
		e.title        = ssel.getColumn(1).getString();
		e.track_number = ssel.getColumn(2).getInt();
		e.disc_number  = ssel.getColumn(3).getInt();
		e.year         = ssel.getColumn(4).getInt();
		e.genre        = ssel.getColumn(5).isNull() ? "" : ssel.getColumn(5).getString();
		e.duration     = ssel.getColumn(6).getDouble();
		e.bitrate      = ssel.getColumn(7).getInt();
		e.file_size    = ssel.getColumn(8).getInt64();
		e.codec        = ssel.getColumn(9).isNull() ? "" : ssel.getColumn(9).getString();
		e.parent_id    = folder_id;
		e.artist       = ssel.getColumn(11).getString();
		e.album        = ssel.getColumn(12).getString();
		e.starred      = ssel.getColumn(13).getString();
		e.width        = ssel.getColumn(14).isNull() ? 0 : ssel.getColumn(14).getInt();
		e.height       = ssel.getColumn(15).isNull() ? 0 : ssel.getColumn(15).getInt();
		e.video_codec  = ssel.getColumn(16).isNull() ? "" : ssel.getColumn(16).getString();
		e.audio_codec  = ssel.getColumn(17).isNull() ? "" : ssel.getColumn(17).getString();
		// A sidecar image beats the album cover: on a loose-file album the
		// album cover belongs to the section, not to this file.
		if (!ssel.getColumn(18).isNull() && ssel.getColumn(18).getString() != "")
			e.cover_art_id = SONG_COVER_ID_BASE + e.id;
		else if (info.album.cover_art_id >= 0)
			e.cover_art_id = info.album.cover_art_id;
		info.songs.push_back(std::move(e));
		}

	return info;
	}

// ---- Play queue / bookmarks ------------------------------------------

void MediaStore::save_play_queue(const std::string& username,
                                  const std::vector<std::string>& song_paths,
                                  const std::string& current_path, int64_t offset_ms,
                                  const std::string& client)
	{
	std::lock_guard<std::mutex> lock(db_mutex_);

	SQLite::Statement uid_q(db_music_, "SELECT id FROM client.users WHERE username = ?");
	uid_q.bind(1, username);
	if (!uid_q.executeStep()) return;
	int user_id = uid_q.getColumn(0).getInt();

	SQLite::Transaction txn(db_music_);

	SQLite::Statement del(db_music_, "DELETE FROM client.play_queue WHERE user_id = ?");
	del.bind(1, user_id);
	del.exec();

	SQLite::Statement ins(db_music_,
		"INSERT INTO client.play_queue (user_id, song_path, position, is_current, offset_ms, client)"
		" VALUES (?, ?, ?, ?, ?, ?)");
	for (int pos = 0; pos < static_cast<int>(song_paths.size()); ++pos) {
		const std::string& sp = song_paths[pos];
		bool is_curr = (!current_path.empty() && sp == current_path);
		ins.bind(1, user_id);
		ins.bind(2, sp);
		ins.bind(3, pos);
		ins.bind(4, is_curr ? 1 : 0);
		ins.bind(5, is_curr ? offset_ms : int64_t(0));
		ins.bind(6, client);
		ins.exec();
		ins.reset();
		}

	txn.commit();
	}

std::optional<MediaStore::PlayQueue> MediaStore::get_play_queue(
	const std::string& username)
	{
	std::lock_guard<std::mutex> lock(db_mutex_);

	// al.folder_id, not s.folder_id — songs on a multi-disc album live in disc
	// subfolders, which have no albums row to resolve cover art or getAlbum against.
	SQLite::Statement q(db_music_,
		"SELECT s.id, s.title, s.track_number, s.disc_number,"
		"       s.year, s.genre, s.duration, s.bitrate,"
		"       s.file_size, s.codec, COALESCE(al.folder_id, s.folder_id),"
		"       COALESCE(a.name,'') AS artist,"
		"       COALESCE(al.title,'') AS album,"
		"       CASE WHEN al.cover_path IS NOT NULL AND al.cover_path != ''"
		"            THEN COALESCE(al.folder_id, s.folder_id) ELSE -1 END AS cover_art_id,"
		"       pq.is_current, pq.offset_ms, pq.client, pq.updated"
		" FROM client.play_queue pq"
		" JOIN client.users u ON u.id = pq.user_id"
		" JOIN songs s ON s.path = pq.song_path"
		" LEFT JOIN albums al ON al.id = s.album_id"
		" LEFT JOIN song_artists sa ON sa.song_id = s.id AND sa.role = 'artist'"
		" LEFT JOIN artists a ON a.id = sa.artist_id"
		" WHERE u.username = ?"
		" ORDER BY pq.position");
	q.bind(1, username);

	PlayQueue pq;
	bool found = false;
	while (q.executeStep()) {
		found = true;
		ChildEntry e;
		e.id           = q.getColumn(0).getInt();
		e.is_dir       = false;
		e.title        = q.getColumn(1).getString();
		e.track_number = q.getColumn(2).getInt();
		e.disc_number  = q.getColumn(3).getInt();
		e.year         = q.getColumn(4).getInt();
		e.genre        = q.getColumn(5).isNull() ? "" : q.getColumn(5).getString();
		e.duration     = q.getColumn(6).getDouble();
		e.bitrate      = q.getColumn(7).getInt();
		e.file_size    = q.getColumn(8).getInt64();
		e.codec        = q.getColumn(9).isNull() ? "" : q.getColumn(9).getString();
		e.parent_id    = q.getColumn(10).getInt();
		e.artist       = q.getColumn(11).getString();
		e.album        = q.getColumn(12).getString();
		e.cover_art_id = q.getColumn(13).getInt();

		bool is_current = q.getColumn(14).getInt() != 0;
		if (is_current) {
			pq.current_id = e.id;
			pq.offset_ms  = q.getColumn(15).getInt64();
			}
		if (pq.client.empty())  pq.client  = q.getColumn(16).isNull() ? "" : q.getColumn(16).getString();
		if (pq.changed.empty()) pq.changed = q.getColumn(17).isNull() ? "" : q.getColumn(17).getString();

		pq.songs.push_back(std::move(e));
		}

	if (!found) return std::nullopt;
	return pq;
	}

void MediaStore::create_bookmark(const std::string& username,
                                  const std::string& song_path, int64_t position_ms,
                                  const std::string& comment)
	{
	std::lock_guard<std::mutex> lock(db_mutex_);

	SQLite::Statement uid_q(db_music_, "SELECT id FROM client.users WHERE username = ?");
	uid_q.bind(1, username);
	if (!uid_q.executeStep()) return;
	int user_id = uid_q.getColumn(0).getInt();

	SQLite::Statement ins(db_music_,
		"INSERT INTO client.bookmarks (user_id, song_path, position, comment, changed)"
		" VALUES (?, ?, ?, ?, CURRENT_TIMESTAMP)"
		" ON CONFLICT(user_id, song_path) DO UPDATE SET"
		"   position = excluded.position,"
		"   comment  = excluded.comment,"
		"   changed  = CURRENT_TIMESTAMP");
	ins.bind(1, user_id);
	ins.bind(2, song_path);
	ins.bind(3, position_ms);
	ins.bind(4, comment);
	ins.exec();
	}

void MediaStore::scrobble(const std::string& username, const std::string& song_path,
                           bool submission, const std::string& client)
	{
	std::lock_guard<std::mutex> lock(db_mutex_);

	SQLite::Statement uid_q(db_music_, "SELECT id FROM client.users WHERE username = ?");
	uid_q.bind(1, username);
	if (!uid_q.executeStep()) return;
	int user_id = uid_q.getColumn(0).getInt();

	if (submission) {
		// Completed play — increment count and record timestamp.
		SQLite::Statement ins(db_music_,
			"INSERT INTO client.play_counts (user_id, song_path, count, last_played)"
			" VALUES (?, ?, 1, CURRENT_TIMESTAMP)"
			" ON CONFLICT(user_id, song_path) DO UPDATE SET"
			"   count       = count + 1,"
			"   last_played = CURRENT_TIMESTAMP");
		ins.bind(1, user_id);
		ins.bind(2, song_path);
		ins.exec();
		} else {
		// Now-playing notification — update or replace the single row.
		SQLite::Statement ins(db_music_,
			"INSERT OR REPLACE INTO client.now_playing (user_id, song_path, client, started)"
			" VALUES (?, ?, ?, CURRENT_TIMESTAMP)");
		ins.bind(1, user_id);
		ins.bind(2, song_path);
		ins.bind(3, client);
		ins.exec();
		}
	}

void MediaStore::add_star(const std::string& username,
                          const std::string& song_path,
                          const std::string& album_folder_path,
                          const std::string& artist_folder_path)
	{
	std::lock_guard<std::mutex> lock(db_mutex_);
	SQLite::Statement uid_q(db_music_, "SELECT id FROM client.users WHERE username = ?");
	uid_q.bind(1, username);
	if (!uid_q.executeStep()) return;
	int user_id = uid_q.getColumn(0).getInt();

	// Store NULL for unused kinds. The read queries discriminate song /
	// album / artist stars via `WHERE <col> IS NOT NULL`, which only
	// works if absent kinds are NULL rather than empty strings.
	auto bind_or_null = [](SQLite::Statement& s, int idx, const std::string& v) {
		if (v.empty()) s.bind(idx);   // NULL
		else           s.bind(idx, v);
		};

	SQLite::Statement ins(db_music_,
		"INSERT OR IGNORE INTO client.stars"
		"   (user_id, song_path, album_folder_path, artist_folder_path)"
		" VALUES (?, ?, ?, ?)");
	ins.bind(1, user_id);
	bind_or_null(ins, 2, song_path);
	bind_or_null(ins, 3, album_folder_path);
	bind_or_null(ins, 4, artist_folder_path);
	ins.exec();
	}

void MediaStore::remove_star(const std::string& username,
                             const std::string& song_path,
                             const std::string& album_folder_path,
                             const std::string& artist_folder_path)
	{
	std::lock_guard<std::mutex> lock(db_mutex_);
	SQLite::Statement uid_q(db_music_, "SELECT id FROM client.users WHERE username = ?");
	uid_q.bind(1, username);
	if (!uid_q.executeStep()) return;
	int user_id = uid_q.getColumn(0).getInt();

	// Build a WHERE that matches the row whose set of non-NULL columns
	// corresponds to the supplied non-empty argument(s). We do not use
	// `IS NULL` blindly for absent args because a star entry has exactly
	// one non-NULL key; matching "all three NULL" would never be a row.
	SQLite::Statement del(db_music_,
		"DELETE FROM client.stars WHERE user_id = ?"
		"   AND COALESCE(song_path,'')          = ?"
		"   AND COALESCE(album_folder_path,'')  = ?"
		"   AND COALESCE(artist_folder_path,'') = ?");
	del.bind(1, user_id);
	del.bind(2, song_path);
	del.bind(3, album_folder_path);
	del.bind(4, artist_folder_path);
	del.exec();
	}

MediaStore::StarredResult MediaStore::get_starred(const std::string& username)
	{
	std::lock_guard<std::mutex> lock(db_mutex_);
	StarredResult result;

	// Starred songs — cover art inherited from album folder if present.
	// al.folder_id, not s.folder_id — songs on a multi-disc album live in disc
	// subfolders, which have no albums row to resolve cover art or getAlbum against.
	SQLite::Statement sq(db_music_,
		"SELECT s.id, s.title, s.track_number, s.disc_number,"
		"       s.year, s.genre, s.duration, s.bitrate,"
		"       s.file_size, s.codec, COALESCE(al.folder_id, s.folder_id),"
		"       COALESCE(a.name,'') AS artist,"
		"       COALESCE(al.title,'') AS album,"
		"       CASE WHEN al.cover_path IS NOT NULL AND al.cover_path != ''"
		"            THEN COALESCE(al.folder_id, s.folder_id) ELSE -1 END AS cover_art_id"
		" FROM client.stars st"
		" JOIN client.users u ON u.id = st.user_id"
		" JOIN songs s ON s.path = st.song_path"
		" LEFT JOIN albums al ON al.id = s.album_id"
		" LEFT JOIN song_artists sa ON sa.song_id = s.id AND sa.role = 'artist'"
		" LEFT JOIN artists a ON a.id = sa.artist_id"
		" WHERE u.username = ? AND st.song_path IS NOT NULL"
		" ORDER BY st.created DESC");
	sq.bind(1, username);
	while (sq.executeStep()) {
		ChildEntry e;
		e.id           = sq.getColumn(0).getInt();
		e.is_dir       = false;
		e.title        = sq.getColumn(1).getString();
		e.track_number = sq.getColumn(2).getInt();
		e.disc_number  = sq.getColumn(3).getInt();
		e.year         = sq.getColumn(4).getInt();
		e.genre        = sq.getColumn(5).isNull() ? "" : sq.getColumn(5).getString();
		e.duration     = sq.getColumn(6).getDouble();
		e.bitrate      = sq.getColumn(7).getInt();
		e.file_size    = sq.getColumn(8).getInt64();
		e.codec        = sq.getColumn(9).isNull() ? "" : sq.getColumn(9).getString();
		e.parent_id    = sq.getColumn(10).getInt();
		e.artist       = sq.getColumn(11).getString();
		e.album        = sq.getColumn(12).getString();
		e.cover_art_id = sq.getColumn(13).getInt();
		result.songs.push_back(std::move(e));
		}

	// Starred albums — stars.album_folder_path stores the album folder path.
	SQLite::Statement aq(db_music_,
		"SELECT f.id,"
		+ ALBUM_ARTIST_ID_SQL +
		"       COALESCE(al.title, f.name) AS title,"
		"       COALESCE(a.name,'') AS artist,"
		"       CASE WHEN al.cover_path IS NOT NULL AND al.cover_path != ''"
		"            THEN f.id ELSE -1 END AS cover_art_id"
		" FROM client.stars st"
		" JOIN client.users u ON u.id = st.user_id"
		" JOIN folders f ON f.path = st.album_folder_path"
		" LEFT JOIN albums al ON al.folder_id = f.id"
		" LEFT JOIN album_artists aa ON aa.album_id = al.id AND aa.role = 'albumartist'"
		" LEFT JOIN artists a ON a.id = aa.artist_id"
		" WHERE u.username = ? AND st.album_folder_path IS NOT NULL"
		" ORDER BY st.created DESC");
	aq.bind(1, username);
	while (aq.executeStep()) {
		ChildEntry e;
		e.id           = aq.getColumn(0).getInt();
		e.parent_id    = aq.getColumn(1).getInt();
		e.is_dir       = true;
		e.title        = aq.getColumn(2).getString();
		e.album        = e.title;
		e.artist       = aq.getColumn(3).getString();
		e.cover_art_id = aq.getColumn(4).getInt();
		result.albums.push_back(std::move(e));
		}

	// Starred artists — stars.artist_folder_path stores the artist folder path.
	SQLite::Statement arq(db_music_,
		"SELECT f.id, f.name"
		" FROM client.stars st"
		" JOIN client.users u ON u.id = st.user_id"
		" JOIN folders f ON f.path = st.artist_folder_path"
		" WHERE u.username = ? AND st.artist_folder_path IS NOT NULL"
		" ORDER BY st.created DESC");
	arq.bind(1, username);
	while (arq.executeStep())
		result.artists.push_back({arq.getColumn(0).getInt(),
		                          arq.getColumn(1).getString()});

	return result;
	}

MediaStore::PlaylistInfo MediaStore::create_playlist(const std::string& username,
                                                      const std::string& name,
                                                      const std::vector<std::string>& song_paths)
	{
	std::lock_guard<std::mutex> lock(db_mutex_);

	SQLite::Statement uid_q(db_music_, "SELECT id FROM client.users WHERE username = ?");
	uid_q.bind(1, username);
	if (!uid_q.executeStep()) return {};
	int user_id = uid_q.getColumn(0).getInt();

	SQLite::Transaction txn(db_music_);

	SQLite::Statement pins(db_music_,
		"INSERT INTO client.playlists (user_id, name) VALUES (?, ?)");
	pins.bind(1, user_id);
	pins.bind(2, name);
	pins.exec();
	int playlist_id = (int)db_music_.getLastInsertRowid();

	SQLite::Statement sins(db_music_,
		"INSERT INTO client.playlist_songs (playlist_id, song_path, position) VALUES (?, ?, ?)");
	for (int pos = 0; pos < (int)song_paths.size(); ++pos) {
		sins.bind(1, playlist_id);
		sins.bind(2, song_paths[pos]);
		sins.bind(3, pos);
		sins.exec();
		sins.reset();
		}

	txn.commit();

	// Read back metadata (timestamps set by SQLite).
	PlaylistInfo pl;
	pl.id        = playlist_id;
	pl.name      = name;
	pl.owner     = username;
	pl.is_public = false;

	SQLite::Statement pmeta(db_music_,
		"SELECT comment, created, updated FROM client.playlists WHERE id = ?");
	pmeta.bind(1, playlist_id);
	if (pmeta.executeStep()) {
		pl.comment = pmeta.getColumn(0).isNull() ? "" : pmeta.getColumn(0).getString();
		pl.created = pmeta.getColumn(1).getString();
		pl.updated = pmeta.getColumn(2).getString();
		}

	// Fetch songs with full metadata, same joins as get_directory.
	// al.folder_id, not s.folder_id — songs on a multi-disc album live in disc
	// subfolders, which have no albums row to resolve cover art or getAlbum against.
	SQLite::Statement sq(db_music_,
		"SELECT s.id, s.title, s.track_number, s.disc_number,"
		"       s.year, s.genre, s.duration, s.bitrate,"
		"       s.file_size, s.codec, COALESCE(al.folder_id, s.folder_id),"
		"       COALESCE(a.name,'') AS artist,"
		"       COALESCE(al.title,'') AS album,"
		"       CASE WHEN al.cover_path IS NOT NULL AND al.cover_path != ''"
		"            THEN COALESCE(al.folder_id, s.folder_id) ELSE -1 END AS cover_art_id"
		" FROM client.playlist_songs ps"
		" JOIN songs s ON s.path = ps.song_path"
		" LEFT JOIN albums al ON al.id = s.album_id"
		" LEFT JOIN song_artists sa ON sa.song_id = s.id AND sa.role = 'artist'"
		" LEFT JOIN artists a ON a.id = sa.artist_id"
		" WHERE ps.playlist_id = ?"
		" ORDER BY ps.position");
	sq.bind(1, playlist_id);
	int total_duration = 0;
	while (sq.executeStep()) {
		ChildEntry e;
		e.id           = sq.getColumn(0).getInt();
		e.is_dir       = false;
		e.title        = sq.getColumn(1).getString();
		e.track_number = sq.getColumn(2).getInt();
		e.disc_number  = sq.getColumn(3).getInt();
		e.year         = sq.getColumn(4).getInt();
		e.genre        = sq.getColumn(5).isNull() ? "" : sq.getColumn(5).getString();
		e.duration     = sq.getColumn(6).getDouble();
		e.bitrate      = sq.getColumn(7).getInt();
		e.file_size    = sq.getColumn(8).getInt64();
		e.codec        = sq.getColumn(9).isNull() ? "" : sq.getColumn(9).getString();
		e.parent_id    = sq.getColumn(10).getInt();
		e.artist       = sq.getColumn(11).getString();
		e.album        = sq.getColumn(12).getString();
		e.cover_art_id = sq.getColumn(13).getInt();
		total_duration += (int)e.duration;
		pl.songs.push_back(std::move(e));
		}

	pl.song_count = (int)pl.songs.size();
	pl.duration   = total_duration;
	return pl;
	}

bool MediaStore::delete_playlist(int playlist_id, const std::string& username)
	{
	std::lock_guard<std::mutex> lock(db_mutex_);
	SQLite::Statement del(db_music_,
		"DELETE FROM client.playlists WHERE id = ?"
		" AND user_id = (SELECT id FROM client.users WHERE username = ?)");
	del.bind(1, playlist_id);
	del.bind(2, username);
	del.exec();
	return db_music_.getChanges() > 0;
	}

std::string MediaStore::abs_path(const std::string& rel) const
	{
	return join_root(rel);
	}

std::string MediaStore::rel_path(const std::string& abs) const
	{
	return strip_root(abs);
	}

bool MediaStore::is_category_folder(int folder_id)
	{
	std::lock_guard<std::mutex> lock(db_mutex_);
	// A root row is never a performer either: loose files directly in a root
	// make the root itself the artist, and looking that up would query
	// MusicBrainz for a thing called "movies" — the failure this function
	// exists to prevent.
	{
	SQLite::Statement r(db_music_,
		"SELECT 1 FROM folders WHERE id = ? AND parent_id IS NULL");
	r.bind(1, folder_id);
	if (r.executeStep()) return true;
	}
	SQLite::Statement q(db_music_,
		"SELECT COALESCE(p.content_type, 'artists')"
		" FROM folders f JOIN folders p ON p.id = f.parent_id"
		" WHERE f.id = ? AND p.parent_id IS NULL");
	q.bind(1, folder_id);
	if (!q.executeStep()) return false;
	return q.getColumn(0).getString() == "categories";
	}

// Brings the folders table's root rows into line with the configuration:
// creates a row per configured library root, stamps its content_type, and
// removes any root that is no longer configured along with everything beneath
// it. Without the removal a dropped root would keep appearing in
// getMusicFolders and its stale artists would still be browsable, because
// every query reaches content by path prefix and nothing else would ever
// revisit it.
void MediaStore::sync_roots()
	{
	std::lock_guard<std::mutex> lock(db_mutex_);
	SQLite::Transaction txn(db_music_);

	std::set<std::string> configured;
	for (const auto& r : roots_) {
		// The uploads root counts as configured — otherwise it would be judged
		// stale below and every user's personal library deleted at startup —
		// but no row is created for it here. Its row appears only when the
		// upload handler scans a batch, and it is filtered out of
		// get_music_folders() so it is never offered as a library to browse.
		configured.insert(r.cfg.name);
		if (r.cfg.type == "uploads") continue;
		int id = upsert_folder(fs::path(r.cfg.path), -1);
		SQLite::Statement upd(db_music_,
			"UPDATE folders SET content_type = ? WHERE id = ?");
		upd.bind(1, r.cfg.type);
		upd.bind(2, id);
		upd.exec();
		}

	std::vector<std::string> stale;
	{
	SQLite::Statement q(db_music_,
		"SELECT path FROM folders WHERE parent_id IS NULL");
	while (q.executeStep()) {
		std::string name = q.getColumn(0).getString();
		if (!configured.count(name)) stale.push_back(name);
		}
	}

	if (stale.empty()) {
		txn.commit();
		return;
		}

	// Foreign keys are deferred to the commit below so the subtree can be torn
	// down in any order. Without it, deleting a parent folder before its
	// children fails, and there is no ordering that satisfies both the
	// folders self-reference and songs.folder_id.
	db_music_.exec("PRAGMA defer_foreign_keys=ON");

	for (const auto& name : stale) {
		std::cout << stamp() << "Root '" << name
		          << "' is no longer configured; removing its entries"
		          << std::endl;
		// Descend the folder tree by id rather than matching a path prefix.
		// A root row is not guaranteed to prefix the paths beneath it — rows
		// written before roots had names store the root's *absolute* path
		// while their descendants are stored relative, so a prefix match finds
		// none of them, leaves them orphaned, and the delete then trips the
		// foreign key. Parentage is the one relationship that is always true.
		static const char* SUBTREE =
			"WITH RECURSIVE sub(id) AS ("
			"  SELECT id FROM folders WHERE path = ?"
			"  UNION ALL"
			"  SELECT f.id FROM folders f JOIN sub ON f.parent_id = sub.id) ";
		for (const char* tail : {
		        "DELETE FROM songs       WHERE folder_id IN (SELECT id FROM sub)",
		        "DELETE FROM albums      WHERE folder_id IN (SELECT id FROM sub)",
		        "DELETE FROM album_info_cache  WHERE folder_id IN (SELECT id FROM sub)",
		        "DELETE FROM artist_info_cache WHERE folder_id IN (SELECT id FROM sub)",
		        "DELETE FROM folders     WHERE id IN (SELECT id FROM sub)" }) {
			SQLite::Statement s(db_music_, std::string(SUBTREE) + tail);
			s.bind(1, name);
			s.exec();
			}
		}

	txn.commit();
	}

std::string MediaStore::not_uploads(const char* col) const
	{
	if (uploads_like_.empty()) return {};
	// The pattern is interpolated rather than bound: these fragments are
	// stitched into larger statements whose bind indices are counted by hand,
	// and a root name is validated at startup to [A-Za-z0-9_-] so it cannot
	// carry a quote.
	return std::string(" AND ") + col + " NOT LIKE '" + uploads_like_ + "'";
	}

const std::vector<MediaStore::Root>& MediaStore::roots() const
	{
	return roots_public_;
	}

const MediaStore::Root* MediaStore::uploads_root() const
	{
	for (const auto& r : roots_)
		if (r.cfg.type == "uploads") return &r.cfg;
	return nullptr;
	}

bool MediaStore::path_is_within_root(const fs::path& candidate) const
	{
	// Within *any* root, not one fixed base.  That generalisation is what
	// makes several roots possible at all, and it does not weaken the check:
	// a symlink inside a root pointing at /etc still fails, because /etc
	// prefixes none of the canonical bases.
	for (const auto& r : roots_)
		if (is_within(candidate, r.canonical)) return true;
	return false;
	}

std::optional<std::string> MediaStore::song_path_by_id(int song_id)
	{
	std::lock_guard<std::mutex> lock(db_mutex_);
	SQLite::Statement q(db_music_, "SELECT path FROM songs WHERE id = ?");
	q.bind(1, song_id);
	if (!q.executeStep()) return std::nullopt;
	return q.getColumn(0).getString();
	}

std::optional<std::string> MediaStore::album_folder_path_by_id(int album_folder_id)
	{
	// External albumId is the folder.id of the album directory.
	std::lock_guard<std::mutex> lock(db_mutex_);
	SQLite::Statement q(db_music_, "SELECT path FROM folders WHERE id = ?");
	q.bind(1, album_folder_id);
	if (!q.executeStep()) return std::nullopt;
	return q.getColumn(0).getString();
	}

std::optional<std::string> MediaStore::artist_folder_path_by_id(int artist_folder_id)
	{
	// External artistId is the folder.id of the artist directory.
	std::lock_guard<std::mutex> lock(db_mutex_);
	SQLite::Statement q(db_music_, "SELECT path FROM folders WHERE id = ?");
	q.bind(1, artist_folder_id);
	if (!q.executeStep()) return std::nullopt;
	return q.getColumn(0).getString();
	}

std::optional<MediaStore::ChildEntry> MediaStore::get_song_entry(int song_id)
	{
	std::lock_guard<std::mutex> lock(db_mutex_);
	// al.folder_id, not s.folder_id — songs on a multi-disc album live in disc
	// subfolders, which have no albums row to resolve cover art or getAlbum against.
	SQLite::Statement q(db_music_,
		"SELECT s.id, s.title, s.track_number, s.disc_number,"
		"       s.year, s.genre, s.duration, s.bitrate,"
		"       s.file_size, s.codec, COALESCE(al.folder_id, s.folder_id),"
		"       COALESCE(a.name,'') AS artist,"
		"       COALESCE(al.title,'') AS album,"
		+ SONG_COVER_ART_SQL +
		"       s.width, s.height, s.video_codec, s.audio_codec"
		" FROM songs s"
		" LEFT JOIN albums al ON al.id = s.album_id"
		" LEFT JOIN song_artists sa ON sa.song_id = s.id AND sa.role = 'artist'"
		" LEFT JOIN artists a ON a.id = sa.artist_id"
		" WHERE s.id = ?");
	q.bind(1, song_id);
	if (!q.executeStep()) return std::nullopt;

	ChildEntry e;
	e.id           = q.getColumn(0).getInt();
	e.is_dir       = false;
	e.title        = q.getColumn(1).getString();
	e.track_number = q.getColumn(2).getInt();
	e.disc_number  = q.getColumn(3).getInt();
	e.year         = q.getColumn(4).getInt();
	e.genre        = q.getColumn(5).isNull() ? "" : q.getColumn(5).getString();
	e.duration     = q.getColumn(6).getDouble();
	e.bitrate      = q.getColumn(7).getInt();
	e.file_size    = q.getColumn(8).getInt64();
	e.codec        = q.getColumn(9).isNull() ? "" : q.getColumn(9).getString();
	e.parent_id    = q.getColumn(10).getInt();
	e.artist       = q.getColumn(11).getString();
	e.album        = q.getColumn(12).getString();
	e.cover_art_id = q.getColumn(13).getInt();
	e.width        = q.getColumn(14).isNull() ? 0 : q.getColumn(14).getInt();
	e.height       = q.getColumn(15).isNull() ? 0 : q.getColumn(15).getInt();
	e.video_codec  = q.getColumn(16).isNull() ? "" : q.getColumn(16).getString();
	e.audio_codec  = q.getColumn(17).isNull() ? "" : q.getColumn(17).getString();
	return e;
	}

MediaStore::SearchResult MediaStore::search(const std::string& query,
                                             int artist_count, int artist_offset,
                                             int album_count,  int album_offset,
                                             int song_count,   int song_offset,
                                             const std::string& personal_user)
	{
	std::lock_guard<std::mutex> lock(db_mutex_);
	SearchResult result;
	std::string pattern = "%" + query + "%";
	std::string path_filter = personal_user.empty()
		? not_uploads("f.path")
		: " AND f.path LIKE ?";
	std::string path_bind = personal_user.empty()
		? ""
		: uploads_prefix_ + personal_user + "/%";
	std::string song_filter = personal_user.empty()
		? not_uploads("s.path")
		: " AND s.path LIKE ?";

	// Artists — folder-level, depth-1 children of the root.
	std::string aq_sql =
		"SELECT f.id, f.name"
		" FROM folders f"
		" WHERE f.parent_id IN (SELECT id FROM folders WHERE parent_id IS NULL)"
		"   AND LOWER(f.name) LIKE LOWER(?)";
	aq_sql += path_filter;
	aq_sql += " ORDER BY f.name COLLATE NOCASE LIMIT ? OFFSET ?";
	SQLite::Statement aq(db_music_, aq_sql);
	int aq_i = 1;
	aq.bind(aq_i++, pattern);
	if (!path_bind.empty()) aq.bind(aq_i++, path_bind);
	aq.bind(aq_i++, artist_count);
	aq.bind(aq_i++, artist_offset);
	while (aq.executeStep()) {
		ChildEntry e;
		e.id        = aq.getColumn(0).getInt();
		e.is_dir    = true;
		e.title     = aq.getColumn(1).getString();
		e.artist    = e.title;
		e.parent_id = -1;
		result.artists.push_back(std::move(e));
		}

	// Albums.
	std::string alq_sql =
		"SELECT f.id,"
		+ ALBUM_ARTIST_ID_SQL +
		"       COALESCE(al.title, f.name),"
		"       COALESCE(a.name,''),"
		"       CASE WHEN al.cover_path IS NOT NULL AND al.cover_path != ''"
		"            THEN f.id ELSE -1 END AS cover_art_id"
		" FROM albums al"
		" JOIN folders f ON f.id = al.folder_id"
		" LEFT JOIN album_artists aa ON aa.album_id = al.id AND aa.role = 'albumartist'"
		" LEFT JOIN artists a ON a.id = aa.artist_id"
		" WHERE LOWER(COALESCE(al.title, f.name)) LIKE LOWER(?)";
	alq_sql += path_filter;
	alq_sql += " ORDER BY al.title COLLATE NOCASE LIMIT ? OFFSET ?";
	SQLite::Statement alq(db_music_, alq_sql);
	int alq_i = 1;
	alq.bind(alq_i++, pattern);
	if (!path_bind.empty()) alq.bind(alq_i++, path_bind);
	alq.bind(alq_i++, album_count);
	alq.bind(alq_i++, album_offset);
	while (alq.executeStep()) {
		ChildEntry e;
		e.id           = alq.getColumn(0).getInt();
		e.parent_id    = alq.getColumn(1).getInt();
		e.is_dir       = true;
		e.title        = alq.getColumn(2).getString();
		e.album        = e.title;
		e.artist       = alq.getColumn(3).getString();
		e.cover_art_id = alq.getColumn(4).getInt();
		result.albums.push_back(std::move(e));
		}

	// Songs — use al.folder_id as parent so the client can call getAlbum directly.
	// For multi-disc albums songs live in disc subfolders, so s.folder_id would be
	// wrong; al.folder_id is always the album root folder that getAlbum expects.
	std::string sq_sql =
		"SELECT s.id, s.title, s.track_number, s.disc_number,"
		"       s.year, s.genre, s.duration, s.bitrate,"
		"       s.file_size, s.codec, COALESCE(al.folder_id, s.folder_id),"
		"       COALESCE(a.name,'') AS artist,"
		"       COALESCE(al.title,'') AS album,"
		"       CASE WHEN al.cover_path IS NOT NULL AND al.cover_path != ''"
		"            THEN COALESCE(al.folder_id, s.folder_id) ELSE -1 END AS cover_art_id"
		" FROM songs s"
		" LEFT JOIN albums al ON al.id = s.album_id"
		" LEFT JOIN song_artists sa ON sa.song_id = s.id AND sa.role = 'artist'"
		" LEFT JOIN artists a ON a.id = sa.artist_id"
		" WHERE LOWER(s.title) LIKE LOWER(?)";
	sq_sql += song_filter;
	sq_sql += " ORDER BY s.title COLLATE NOCASE LIMIT ? OFFSET ?";
	SQLite::Statement sq(db_music_, sq_sql);
	int sq_i = 1;
	sq.bind(sq_i++, pattern);
	if (!path_bind.empty()) sq.bind(sq_i++, path_bind);
	sq.bind(sq_i++, song_count);
	sq.bind(sq_i++, song_offset);
	while (sq.executeStep()) {
		ChildEntry e;
		e.id           = sq.getColumn(0).getInt();
		e.is_dir       = false;
		e.title        = sq.getColumn(1).getString();
		e.track_number = sq.getColumn(2).getInt();
		e.disc_number  = sq.getColumn(3).getInt();
		e.year         = sq.getColumn(4).getInt();
		e.genre        = sq.getColumn(5).isNull() ? "" : sq.getColumn(5).getString();
		e.duration     = sq.getColumn(6).getDouble();
		e.bitrate      = sq.getColumn(7).getInt();
		e.file_size    = sq.getColumn(8).getInt64();
		e.codec        = sq.getColumn(9).isNull() ? "" : sq.getColumn(9).getString();
		e.parent_id    = sq.getColumn(10).getInt();
		e.artist       = sq.getColumn(11).getString();
		e.album        = sq.getColumn(12).getString();
		e.cover_art_id = sq.getColumn(13).getInt();
		result.songs.push_back(std::move(e));
		}

	return result;
	}

std::vector<MediaStore::BookmarkInfo> MediaStore::get_bookmarks(
	const std::string& username)
	{
	std::lock_guard<std::mutex> lock(db_mutex_);
	// al.folder_id, not s.folder_id — songs on a multi-disc album live in disc
	// subfolders, which have no albums row to resolve cover art or getAlbum against.
	SQLite::Statement q(db_music_,
		"SELECT s.id, s.title, s.track_number, s.disc_number,"
		"       s.year, s.genre, s.duration, s.bitrate,"
		"       s.file_size, s.codec, COALESCE(al.folder_id, s.folder_id),"
		"       COALESCE(a.name,'') AS artist,"
		"       COALESCE(al.title,'') AS album,"
		"       CASE WHEN al.cover_path IS NOT NULL AND al.cover_path != ''"
		"            THEN COALESCE(al.folder_id, s.folder_id) ELSE -1 END AS cover_art_id,"
		"       b.position, COALESCE(b.comment,''), b.created, b.changed,"
		"       u.username"
		" FROM client.bookmarks b"
		" JOIN client.users u ON u.id = b.user_id"
		" JOIN songs s ON s.path = b.song_path"
		" LEFT JOIN albums al ON al.id = s.album_id"
		" LEFT JOIN song_artists sa ON sa.song_id = s.id AND sa.role = 'artist'"
		" LEFT JOIN artists a ON a.id = sa.artist_id"
		" WHERE u.username = ?"
		" ORDER BY b.created");
	q.bind(1, username);

	std::vector<BookmarkInfo> result;
	while (q.executeStep()) {
		BookmarkInfo bm;
		bm.entry.id           = q.getColumn(0).getInt();
		bm.entry.is_dir       = false;
		bm.entry.title        = q.getColumn(1).getString();
		bm.entry.track_number = q.getColumn(2).getInt();
		bm.entry.disc_number  = q.getColumn(3).getInt();
		bm.entry.year         = q.getColumn(4).getInt();
		bm.entry.genre        = q.getColumn(5).isNull() ? "" : q.getColumn(5).getString();
		bm.entry.duration     = q.getColumn(6).getDouble();
		bm.entry.bitrate      = q.getColumn(7).getInt();
		bm.entry.file_size    = q.getColumn(8).getInt64();
		bm.entry.codec        = q.getColumn(9).isNull() ? "" : q.getColumn(9).getString();
		bm.entry.parent_id    = q.getColumn(10).getInt();
		bm.entry.artist       = q.getColumn(11).getString();
		bm.entry.album        = q.getColumn(12).getString();
		bm.entry.cover_art_id = q.getColumn(13).getInt();
		bm.position           = q.getColumn(14).getInt64();
		bm.comment            = q.getColumn(15).getString();
		bm.created            = q.getColumn(16).getString();
		bm.changed            = q.getColumn(17).getString();
		bm.username           = q.getColumn(18).getString();
		result.push_back(std::move(bm));
		}
	return result;
	}

bool MediaStore::delete_bookmark(const std::string& username, const std::string& song_path)
	{
	std::lock_guard<std::mutex> lock(db_mutex_);
	SQLite::Statement del(db_music_,
		"DELETE FROM client.bookmarks WHERE song_path = ?"
		" AND user_id = (SELECT id FROM client.users WHERE username = ?)");
	del.bind(1, song_path);
	del.bind(2, username);
	del.exec();
	return db_music_.getChanges() > 0;
	}

bool MediaStore::update_playlist(int playlist_id, const std::string& username,
                                  const std::optional<std::string>& name,
                                  const std::optional<std::string>& comment,
                                  const std::optional<bool>& is_public,
                                  const std::vector<std::string>& song_paths_to_add,
                                  const std::vector<int>& indices_to_remove)
	{
	std::lock_guard<std::mutex> lock(db_mutex_);

	// Verify ownership.
	SQLite::Statement own(db_music_,
		"SELECT p.id FROM client.playlists p"
		" JOIN client.users u ON u.id = p.user_id"
		" WHERE p.id = ? AND u.username = ?");
	own.bind(1, playlist_id);
	own.bind(2, username);
	if (!own.executeStep()) return false;

	// Read surviving song_paths in position order, then drop requested indices.
	SQLite::Statement sel(db_music_,
		"SELECT song_path FROM client.playlist_songs WHERE playlist_id = ? ORDER BY position");
	sel.bind(1, playlist_id);
	std::vector<std::string> kept;
	while (sel.executeStep())
		kept.push_back(sel.getColumn(0).getString());

	// Remove in descending index order to avoid shifting.
	std::vector<int> sorted_remove = indices_to_remove;
	std::sort(sorted_remove.rbegin(), sorted_remove.rend());
	for (int idx : sorted_remove)
		if (idx >= 0 && idx < (int)kept.size())
			kept.erase(kept.begin() + idx);

	for (const auto& sp : song_paths_to_add)
		kept.push_back(sp);

	SQLite::Transaction txn(db_music_);

	// Apply metadata changes.
	if (name)      {
		SQLite::Statement q(db_music_, "UPDATE client.playlists SET name=? WHERE id=?");
		q.bind(1, *name); q.bind(2, playlist_id); q.exec();
		}
	if (comment)   {
		SQLite::Statement q(db_music_, "UPDATE client.playlists SET comment=? WHERE id=?");
		q.bind(1, *comment); q.bind(2, playlist_id); q.exec();
		}
	if (is_public) {
		SQLite::Statement q(db_music_, "UPDATE client.playlists SET is_public=? WHERE id=?");
		q.bind(1, *is_public ? 1 : 0); q.bind(2, playlist_id); q.exec();
		}
	{
	SQLite::Statement q(db_music_, "UPDATE client.playlists SET updated=CURRENT_TIMESTAMP WHERE id=?");
	q.bind(1, playlist_id); q.exec();
	}

	// Replace song list.
	SQLite::Statement del(db_music_, "DELETE FROM client.playlist_songs WHERE playlist_id=?");
	del.bind(1, playlist_id);
	del.exec();

	SQLite::Statement ins(db_music_,
		"INSERT INTO client.playlist_songs (playlist_id, song_path, position) VALUES (?,?,?)");
	for (int pos = 0; pos < (int)kept.size(); ++pos) {
		ins.bind(1, playlist_id);
		ins.bind(2, kept[pos]);
		ins.bind(3, pos);
		ins.exec();
		ins.reset();
		}

	txn.commit();
	return true;
	}

std::vector<MediaStore::PlaylistInfo> MediaStore::get_playlists(const std::string& username)
	{
	std::lock_guard<std::mutex> lock(db_mutex_);

	// Song counts and total durations via aggregates; no song rows returned.
	SQLite::Statement q(db_music_,
		"SELECT p.id, p.name, COALESCE(p.comment,''), u.username, p.is_public,"
		"       COUNT(ps.song_path), COALESCE(SUM(s.duration),0),"
		"       p.created, p.updated"
		" FROM client.playlists p"
		" JOIN client.users u ON u.id = p.user_id"
		" LEFT JOIN client.playlist_songs ps ON ps.playlist_id = p.id"
		" LEFT JOIN songs s ON s.path = ps.song_path"
		" WHERE u.username = ?"
		" GROUP BY p.id"
		" ORDER BY p.name COLLATE NOCASE");
	q.bind(1, username);

	std::vector<PlaylistInfo> result;
	while (q.executeStep()) {
		PlaylistInfo pl;
		pl.id        = q.getColumn(0).getInt();
		pl.name      = q.getColumn(1).getString();
		pl.comment   = q.getColumn(2).getString();
		pl.owner     = q.getColumn(3).getString();
		pl.is_public = q.getColumn(4).getInt() != 0;
		pl.song_count= q.getColumn(5).getInt();
		pl.duration  = q.getColumn(6).getInt();
		pl.created   = q.getColumn(7).getString();
		pl.updated   = q.getColumn(8).getString();
		result.push_back(std::move(pl));
		}
	return result;
	}

std::optional<MediaStore::PlaylistInfo> MediaStore::get_playlist(int playlist_id,
                                                                    const std::string& username)
	{
	std::lock_guard<std::mutex> lock(db_mutex_);

	SQLite::Statement pmeta(db_music_,
		"SELECT p.name, COALESCE(p.comment,''), u.username, p.is_public,"
		"       p.created, p.updated"
		" FROM client.playlists p"
		" JOIN client.users u ON u.id = p.user_id"
		" WHERE p.id = ?");
	pmeta.bind(1, playlist_id);
	if (!pmeta.executeStep()) return std::nullopt;

	PlaylistInfo pl;
	pl.id        = playlist_id;
	pl.name      = pmeta.getColumn(0).getString();
	pl.comment   = pmeta.getColumn(1).getString();
	pl.owner     = pmeta.getColumn(2).getString();
	pl.is_public = pmeta.getColumn(3).getInt() != 0;
	pl.created   = pmeta.getColumn(4).getString();
	pl.updated   = pmeta.getColumn(5).getString();

	const std::string star_col  = username.empty()
		? ", '' AS starred"
		: ", COALESCE(st.created, '') AS starred";
	const std::string star_join = username.empty()
		? ""
		: " LEFT JOIN client.stars st ON st.song_path = s.path"
		  " AND st.user_id = (SELECT id FROM client.users WHERE username = ?)";

	// al.folder_id, not s.folder_id — songs on a multi-disc album live in disc
	// subfolders, which have no albums row to resolve cover art or getAlbum against.
	std::string pl_sql =
		"SELECT s.id, s.title, s.track_number, s.disc_number,"
		"       s.year, s.genre, s.duration, s.bitrate,"
		"       s.file_size, s.codec, COALESCE(al.folder_id, s.folder_id),"
		"       COALESCE(a.name,'') AS artist,"
		"       COALESCE(al.title,'') AS album,"
		"       CASE WHEN al.cover_path IS NOT NULL AND al.cover_path != ''"
		"            THEN COALESCE(al.folder_id, s.folder_id) ELSE -1 END AS cover_art_id"
		+ star_col +
		" FROM client.playlist_songs ps"
		" JOIN songs s ON s.path = ps.song_path"
		" LEFT JOIN albums al ON al.id = s.album_id"
		" LEFT JOIN song_artists sa ON sa.song_id = s.id AND sa.role = 'artist'"
		" LEFT JOIN artists a ON a.id = sa.artist_id"
		+ star_join +
		" WHERE ps.playlist_id = ?"
		" ORDER BY ps.position";

	SQLite::Statement sq(db_music_, pl_sql);
	int idx = 1;
	if (!username.empty())
		sq.bind(idx++, username);            // star_join user_id subquery
	sq.bind(idx, playlist_id);
	int total_duration = 0;
	while (sq.executeStep()) {
		ChildEntry e;
		e.id           = sq.getColumn(0).getInt();
		e.is_dir       = false;
		e.title        = sq.getColumn(1).getString();
		e.track_number = sq.getColumn(2).getInt();
		e.disc_number  = sq.getColumn(3).getInt();
		e.year         = sq.getColumn(4).getInt();
		e.genre        = sq.getColumn(5).isNull() ? "" : sq.getColumn(5).getString();
		e.duration     = sq.getColumn(6).getDouble();
		e.bitrate      = sq.getColumn(7).getInt();
		e.file_size    = sq.getColumn(8).getInt64();
		e.codec        = sq.getColumn(9).isNull() ? "" : sq.getColumn(9).getString();
		e.parent_id    = sq.getColumn(10).getInt();
		e.artist       = sq.getColumn(11).getString();
		e.album        = sq.getColumn(12).getString();
		e.cover_art_id = sq.getColumn(13).getInt();
		e.starred      = sq.getColumn(14).getString();
		total_duration += (int)e.duration;
		pl.songs.push_back(std::move(e));
		}

	pl.song_count = (int)pl.songs.size();
	pl.duration   = total_duration;
	return pl;
	}

bool MediaStore::update_song_meta(int song_id,
                                   const std::optional<std::string>& title,
                                   const std::optional<int>& track_number,
                                   const std::optional<int>& year,
                                   const std::optional<int>& disc_number)
	{
	std::lock_guard<std::mutex> lock(db_mutex_);

	// Verify the song exists before touching anything.
	SQLite::Statement sel(db_music_, "SELECT 1 FROM songs WHERE id = ?");
	sel.bind(1, song_id);
	if (!sel.executeStep()) return false;

	if (title) {
		SQLite::Statement q(db_music_, "UPDATE songs SET title = ? WHERE id = ?");
		q.bind(1, *title);
		q.bind(2, song_id);
		q.exec();
		}
	if (track_number) {
		SQLite::Statement q(db_music_, "UPDATE songs SET track_number = ? WHERE id = ?");
		q.bind(1, *track_number);
		q.bind(2, song_id);
		q.exec();
		}
	if (year) {
		SQLite::Statement q(db_music_, "UPDATE songs SET year = ? WHERE id = ?");
		q.bind(1, *year);
		q.bind(2, song_id);
		q.exec();

		// Propagate to the album: use the same strategy as the scanner —
		// take the year of the first track (disc/track order) with year > 0.
		SQLite::Statement upd(db_music_,
			"UPDATE albums SET year = ("
			"  SELECT s.year FROM songs s"
			"  WHERE s.album_id = (SELECT album_id FROM songs WHERE id = ?)"
			"    AND s.year > 0"
			"  ORDER BY s.disc_number, s.track_number LIMIT 1"
			") WHERE id = (SELECT album_id FROM songs WHERE id = ?)");
		upd.bind(1, song_id);
		upd.bind(2, song_id);
		upd.exec();
		}
	if (disc_number) {
		SQLite::Statement q(db_music_,
			"UPDATE songs SET disc_number = ? WHERE id = ?");
		q.bind(1, *disc_number);
		q.bind(2, song_id);
		q.exec();
		}
	return true;
	}

bool MediaStore::set_cover_art_path(int folder_id, const std::string& path)
	{
	std::lock_guard<std::mutex> lock(db_mutex_);
	SQLite::Statement q(db_music_,
		"UPDATE albums SET cover_path = ? WHERE folder_id = ?");
	q.bind(1, path);
	q.bind(2, folder_id);
	q.exec();
	return db_music_.getChanges() > 0;
	}

std::string MediaStore::get_setting(const std::string& key,
                                    const std::string& default_val)
	{
	std::lock_guard<std::mutex> lock(db_mutex_);
	SQLite::Statement q(db_music_,
		"SELECT value FROM client.settings WHERE key = ?");
	q.bind(1, key);
	if (!q.executeStep()) return default_val;
	return q.getColumn(0).getString();
	}

void MediaStore::set_setting(const std::string& key, const std::string& value)
	{
	std::lock_guard<std::mutex> lock(db_mutex_);
	SQLite::Statement s(db_music_,
		"INSERT OR REPLACE INTO client.settings (key, value) VALUES (?, ?)");
	s.bind(1, key);
	s.bind(2, value);
	s.exec();
	}

