#include "mediastore.hh"
#include "stamp.hh"
#include "md5.hh"

#include <iostream>
#include <algorithm>
#include <chrono>
#include <regex>
#include <set>
#include <unordered_map>

#include <taglib/fileref.h>
#include <tfilestream.h>
#include <taglib/tag.h>
#include <taglib/audioproperties.h>
#include <tpropertymap.h>

namespace fs = std::filesystem;

static const std::set<std::string> AUDIO_EXTENSIONS = {
	".flac", ".mp3", ".ogg", ".oga", ".m4a", ".aac", ".wav", ".opus", ".wma"
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

static std::string find_cover(const fs::path& dir)
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
	for (auto& entry : fs::recursive_directory_iterator(dir)) {
		if (!entry.is_regular_file()) continue;
		std::string fname = entry.path().filename().string();
		if (iends_with(fname, ".jpg") || iends_with(fname, ".jpeg") || iends_with(fname, ".png"))
			return entry.path().string();
		}
	return "";
	}

// Returns sorted list of image paths in dir (recursive), excluding cover_path.
static std::vector<std::string> find_extra_images(const fs::path& dir,
                                                   const std::string& cover_path)
	{
	static const std::set<std::string> IMG_EXT = {".jpg", ".jpeg", ".png"};
	std::vector<std::string> result;
	try {
		for (auto& entry : fs::recursive_directory_iterator(dir)) {
			if (!entry.is_regular_file()) continue;
			std::string ext = entry.path().extension().string();
			std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
			if (!IMG_EXT.count(ext)) continue;
			if (entry.path().string() == cover_path) continue;
			result.push_back(entry.path().string());
			}
		}
	catch (...) {}
	std::sort(result.begin(), result.end());
	return result;
	}

static bool is_audio_file(const fs::path& p)
	{
	std::string ext = p.extension().string();
	std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
	return AUDIO_EXTENSIONS.count(ext) > 0;
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
// (client.*) are stored as paths relative to music_root_; strip_root() trims
// an absolute path to that form on write, and join_root() composes the
// absolute form when a filesystem call needs it.

static std::string strip_root(const std::string& abs, const std::string& root_slash)
	{
	if (abs.size() >= root_slash.size()
	    && abs.compare(0, root_slash.size(), root_slash) == 0)
		return abs.substr(root_slash.size());
	return abs;  // outside music_root — shouldn't happen, but pass through
	}

static std::string join_root(const std::string& rel, const std::string& root_slash)
	{
	if (rel.empty()) return rel;
	// Defensive: if the caller already passed an absolute path, leave it.
	if (rel.size() >= root_slash.size()
	    && rel.compare(0, root_slash.size(), root_slash) == 0)
		return rel;
	return root_slash + rel;
	}

// Defence-in-depth: returns true iff the canonicalised candidate sits within
// the (already-canonicalised) base. Used to refuse any filesystem operation on
// a path that — after symlink resolution — escapes music_root_. The base is
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

MediaStore::MediaStore(const std::string& db_path, const std::string& music_root)
	: music_root_(music_root),
	  db_music_(derive_path(db_path, "-music"), SQLite::OPEN_READWRITE | SQLite::OPEN_CREATE)
	{
	// Normalise music_root_: strip a trailing '/' so it never has one, and
	// derive music_root_slash_ as the form that always does. Used as the
	// hinge between music-root-relative paths (stored in folders.path /
	// songs.path / albums.cover_path / client.*) and absolute filesystem
	// paths (used by std::filesystem / TagLib / ffmpeg).
	while (music_root_.size() > 1 && music_root_.back() == '/')
		music_root_.pop_back();
	music_root_slash_ = music_root_ + "/";

	// Compute the canonical music_root once. path_is_within_root() compares
	// against this, so symlinks within the tree are checked at every file open.
	std::error_code ec;
	music_root_canonical_ = fs::weakly_canonical(fs::path(music_root_), ec);
	if (ec)
		music_root_canonical_ = fs::path(music_root_);

	std::string client_path = derive_path(db_path, "-client");
	db_music_.exec("PRAGMA journal_mode=WAL");
	db_music_.exec("PRAGMA foreign_keys=ON");
	db_music_.exec("ATTACH DATABASE '" + client_path + "' AS client");
	db_music_.exec("PRAGMA client.journal_mode=WAL");
	db_music_.exec("PRAGMA client.foreign_keys=ON");
	create_schema();
	}

void MediaStore::create_schema()
	{
	SQLite::Transaction txn(db_music_);

	// Music library tables (gaindrive-music.db, main schema).
	db_music_.exec(R"(
		CREATE TABLE IF NOT EXISTS folders (
			id           INTEGER PRIMARY KEY,
			parent_id    INTEGER REFERENCES folders(id),
			path         TEXT NOT NULL UNIQUE,    -- relative to music_root; root row stores ""
			name         TEXT NOT NULL,
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
			cover_path     TEXT,                  -- relative to music_root
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
			path                TEXT NOT NULL UNIQUE, -- relative to music_root
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
		-- to music_root, never by row id. This keeps client data alive
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
	std::cout << stamp() << "Scan started: " << music_root_ << std::endl;

	// Ensure the root folder row exists before per-artist work begins.
	{
	std::lock_guard<std::mutex> lock(db_mutex_);
	SQLite::Transaction txn(db_music_);
	upsert_folder(fs::path(music_root_), -1);
	txn.commit();
	}

	// Collect paths to scan (absolute, since scan_artist_dir needs absolute for
	// fs ops): artist dirs present on disk, plus any still in the DB (so
	// deleted artist directories get pruned).  The root folder is stored
	// with path = "" in the relative-path scheme.
	std::set<fs::path> to_scan;
	for (auto& e : fs::directory_iterator(music_root_)) {
		if (e.is_directory())
			to_scan.insert(e.path());
		}
	{
	std::lock_guard<std::mutex> lock(db_mutex_);
	SQLite::Statement s(db_music_,
		"SELECT path FROM folders"
		" WHERE parent_id = (SELECT id FROM folders WHERE path = ?)"
		"   AND path != ?");
	s.bind(1, std::string(""));
	s.bind(2, std::string(""));
	while (s.executeStep())
		to_scan.insert(fs::path(music_root_slash_ + s.getColumn(0).getString()));
	}

	// Process each artist in its own transaction so db_mutex_ is released
	// between artists and API handlers can run during the scan.
	for (auto& artist_path : to_scan)
		scan_artist_dir(artist_path);

	std::cout << stamp() << "Scan complete" << std::endl;
	}

void MediaStore::scan_dirs(const std::set<std::string>& dirs)
	{
	// dirs are paths relative to music_root_; "" means the root itself
	// (queue overflow or other situation where we lost track of what changed).
	if (dirs.count("")) {
		scan();
		return;
		}
	for (auto& d : dirs)
		scan_artist_dir(fs::path(music_root_slash_ + d));
	}

// Per-song data collected in Phases 1–3, consumed in Phase 4.
struct SongReadData {
	std::string path;
	std::string folder_path;   // disc dir or album dir
	int64_t     mtime       = 0;
	int64_t     file_size   = 0;
	std::string codec;
	int         disc_number = 0;
	bool        changed     = false;
	// populated in Phase 3 only when changed == true:
	std::string title;
	int         track_nr    = 0;
	int         year        = 0;
	std::string genre;
	double      duration    = 0.0;
	int         bitrate     = 0;
	int         sr          = 0;
	int         channels    = 0;
	};

struct AlbumReadData {
	std::string               path;
	std::string               title;
	std::string               cover;
	std::vector<std::string>  disc_paths;  // sorted; index+1 = disc number
	std::vector<SongReadData> songs;
	};

// DB-only counterpart of upsert_song(); all slow I/O has already happened.
// Caller must hold db_mutex_ and an open transaction. sdat.path is absolute
// (Phase 1/3 use it for TagLib I/O); this function strips to music-root-
// relative for all DB binds.
static void upsert_song_with_data(SQLite::Database& db, const SongReadData& sdat,
                                   int album_id, int folder_id, int artist_id,
                                   const std::string& music_root_slash)
	{
	std::string rel_path = strip_root(sdat.path, music_root_slash);

	if (!sdat.changed) {
		if (sdat.disc_number > 0) {
			SQLite::Statement upd(db,
				"UPDATE songs SET disc_number=? WHERE path=? AND disc_number!=?");
			upd.bind(1, sdat.disc_number);
			upd.bind(2, rel_path);
			upd.bind(3, sdat.disc_number);
			upd.exec();
			}
		return;
		}

	static const std::regex track_prefix(R"(^\d+[. -]+)");
	std::string title = std::regex_replace(sdat.title, track_prefix, "");

	SQLite::Statement ins(db,
		"INSERT OR REPLACE INTO songs"
		" (album_id, folder_id, path, filename, title, track_number, disc_number,"
		"  year, genre, duration, bitrate, sample_rate, channels, codec,"
		"  file_size, file_modified, last_scanned)"
		" VALUES (?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,CURRENT_TIMESTAMP)");
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
//   3. TagLib reads for changed files only (no lock)
//   4. Short write txns: one per album, plus the unvisited-mark and the
//      artist-level prune.  Per-album commits keep the mutex hold time
//      bounded so REST handlers stay responsive during the scan.
void MediaStore::scan_artist_dir(const fs::path& artist_path)
	{
	// SQL `path LIKE ?` queries below compare against the relative-form column,
	// so the prefix must also be relative (e.g. "Artist Name/%").
	std::string prefix =
		strip_root(artist_path.string(), music_root_slash_) + "/%";
	bool exists = fs::is_directory(artist_path);

	std::cout << stamp() << "Rescan: " << artist_path.filename().string()
	          << (exists ? "" : " (removed)") << std::endl;

	// ---- Phase 1: walk disk (no lock) ----
	std::vector<AlbumReadData> albums;
	if (exists) {
		for (auto& album_entry : fs::directory_iterator(artist_path)) {
			if (!album_entry.is_directory()) continue;

			AlbumReadData adat;
			adat.path  = album_entry.path().string();
			adat.title = album_entry.path().filename().string();
			std::replace(adat.title.begin(), adat.title.end(), '_', ' ');
			adat.cover = find_cover(album_entry.path());

			std::vector<fs::directory_entry> disc_dirs;
			std::vector<fs::path>            direct_files;
			for (auto& e : fs::directory_iterator(album_entry.path())) {
				if      (e.is_directory())                               disc_dirs.push_back(e);
				else if (e.is_regular_file() && is_audio_file(e.path())) direct_files.push_back(e.path());
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
					if (!te.is_regular_file() || !is_audio_file(te.path())) continue;
					SongReadData sdat;
					sdat.path        = te.path().string();
					sdat.folder_path = disc_dirs[dn].path().string();
					sdat.mtime       = mtime_of(te.path());
					sdat.file_size   = static_cast<int64_t>(fs::file_size(te.path()));
					sdat.codec       = te.path().extension().string().substr(1);
					std::transform(sdat.codec.begin(), sdat.codec.end(),
					               sdat.codec.begin(), ::tolower);
					sdat.disc_number = dn + 1;
					adat.songs.push_back(std::move(sdat));
					}
				}
			for (auto& p : direct_files) {
				SongReadData sdat;
				sdat.path        = p.string();
				sdat.folder_path = adat.path;
				sdat.mtime       = mtime_of(p);
				sdat.file_size   = static_cast<int64_t>(fs::file_size(p));
				sdat.codec       = p.extension().string().substr(1);
				std::transform(sdat.codec.begin(), sdat.codec.end(),
				               sdat.codec.begin(), ::tolower);
				sdat.disc_number = 0;
				adat.songs.push_back(std::move(sdat));
				}

			albums.push_back(std::move(adat));
			}
		}

	// ---- Phase 2: brief read lock — identify changed files ----
	// known_mtimes is keyed by the same absolute-path form as sdat.path so the
	// per-song lookup below stays a direct comparison. The DB column is
	// relative; compose absolute via music_root_slash_ on the way in.
	std::unordered_map<std::string, int64_t> known_mtimes;
	{
	std::lock_guard<std::mutex> lock(db_mutex_);
	SQLite::Statement q(db_music_,
		"SELECT path, file_modified FROM songs WHERE path LIKE ?");
	q.bind(1, prefix);
	while (q.executeStep())
		known_mtimes[music_root_slash_ + q.getColumn(0).getString()]
			= q.getColumn(1).getInt64();
	}

	for (auto& adat : albums)
		for (auto& sdat : adat.songs) {
			auto it = known_mtimes.find(sdat.path);
			sdat.changed = (it == known_mtimes.end() || it->second != sdat.mtime);
			}

	// ---- Phase 3: TagLib reads for changed files only (no lock) ----
	for (auto& adat : albums) {
		for (auto& sdat : adat.songs) {
			if (!sdat.changed) continue;

			// Defence-in-depth: refuse to open any file that — after symlink
			// resolution — sits outside music_root_.
			if (!is_within(sdat.path, music_root_canonical_)) {
				std::cout << stamp() << "scan: skipping file outside music_root: "
				          << sdat.path << std::endl;
				continue;
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
		}

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

	if (exists) {
		int root_id, artist_folder_id, artist_id;
		{
		std::lock_guard<std::mutex> lock(db_mutex_);
		SQLite::Transaction txn(db_music_);
		root_id          = upsert_folder(fs::path(music_root_), -1);
		artist_folder_id = upsert_folder(artist_path, root_id);
		artist_id        = upsert_artist(artist_path.filename().string());
		txn.commit();
		}

		for (auto& adat : albums) {
			{
			std::lock_guard<std::mutex> lock(db_mutex_);
			SQLite::Transaction txn(db_music_);

			int album_folder_id = upsert_folder(fs::path(adat.path), artist_folder_id);
			int album_id        = upsert_album(album_folder_id, adat.title, artist_id, 0, "");

			if (!adat.cover.empty()) {
				SQLite::Statement upd(db_music_,
					"UPDATE albums SET cover_path = ? WHERE id = ?");
				upd.bind(1, strip_root(adat.cover, music_root_slash_));
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

			for (auto& sdat : adat.songs) {
				auto fit = fid_map.find(sdat.folder_path);
				int  fid = (fit != fid_map.end()) ? fit->second : album_folder_id;
				upsert_song_with_data(db_music_, sdat, album_id, fid, artist_id,
				                       music_root_slash_);
				}

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
		}

	// Prune stale entries within this artist's subtree.  Anything still
	// last_scanned IS NULL after the album commits above is genuinely gone.
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
	SQLite::Statement s(db_music_,
		"DELETE FROM artists WHERE id NOT IN"
		" (SELECT DISTINCT artist_id FROM album_artists)");
	s.exec();
	}
	if (!exists) {
		std::string artist_rel = strip_root(artist_path.string(), music_root_slash_);
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

// ---- upsert helpers ---------------------------------------------------

int MediaStore::upsert_folder(const fs::path& path, int parent_id)
	{
	// Caller passes an absolute path (from fs walks); strip to music-root-
	// relative form for storage. The root folder ends up stored as "".
	std::string path_str = strip_root(path.string(), music_root_slash_);
	std::string name     = path.filename().string();
	if (name.empty()) name = path.string();  // for the root itself

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

void MediaStore::upsert_song(const fs::path& path, int album_id, int folder_id,
                              int artist_id, int disc_number)
	{
	int64_t mtime = mtime_of(path);

	// path arrives absolute (from fs walk); the DB stores it relative.
	std::string rel_path = strip_root(path.string(), music_root_slash_);

	// Skip if the file hasn't changed since last scan.
	SQLite::Statement chk(db_music_,
		"SELECT file_modified FROM songs WHERE path = ?");
	chk.bind(1, rel_path);
	if (chk.executeStep()) {
		if (chk.getColumn(0).getInt64() == mtime) {
			// Still update disc_number if it was folder-derived — folders may be reordered.
			if (disc_number > 0) {
				SQLite::Statement upd(db_music_,
					"UPDATE songs SET disc_number=? WHERE path=? AND disc_number!=?");
				upd.bind(1, disc_number);
				upd.bind(2, rel_path);
				upd.bind(3, disc_number);
				upd.exec();
				}
			return;
			}
		}

	// Defence-in-depth: refuse to open any file that — after symlink
	// resolution — sits outside music_root_.
	if (!is_within(path, music_root_canonical_)) {
		std::cout << stamp() << "scan: skipping file outside music_root: "
		          << path.string() << std::endl;
		return;
		}

	// Read tags with taglib — open read-only so we never mutate media files.
	TagLib::FileStream stream(path.c_str(), true /* readOnly */);
	TagLib::FileRef    f(&stream);
	std::string title    = path.stem().string();  // fallback: filename stem
	int         track_nr = 0;
	int         year     = 0;
	std::string genre;
	double      duration = 0;
	int         bitrate  = 0;
	int         sr       = 0;
	int         channels = 0;

	if (!f.isNull() && f.tag()) {
		auto* t = f.tag();
		if (!t->title().isEmpty())
			title = t->title().toCString(true);
		track_nr = static_cast<int>(t->track());
		year     = static_cast<int>(t->year());
		if (!t->genre().isEmpty())
			genre = t->genre().toCString(true);
		}
	// Fall back to DISCNUMBER file tag when folder structure gives no disc info.
	if (disc_number == 0 && !f.isNull()) {
		auto props = f.file()->properties();
		auto it = props.find("DISCNUMBER");
		if (it != props.end() && !it->second.isEmpty()) {
			try { disc_number = it->second.front().toInt(); }
			catch (...) {}
			}
		}
	// Strip leading track-number prefixes (e.g. "01 - ", "1. ") from title.
	// Requires at least one separator char so bare numbers/years are left alone.
	static const std::regex track_prefix(R"(^\d+[. -]+)");
	title = std::regex_replace(title, track_prefix, "");

	if (!f.isNull() && f.audioProperties()) {
		auto* ap = f.audioProperties();
		duration = ap->lengthInSeconds();
		bitrate  = ap->bitrate();
		sr       = ap->sampleRate();
		channels = ap->channels();
		}

	int64_t file_size = static_cast<int64_t>(fs::file_size(path));
	std::string codec = path.extension().string().substr(1);  // strip leading dot
	std::transform(codec.begin(), codec.end(), codec.begin(), ::tolower);

	SQLite::Statement ins(db_music_,
		"INSERT OR REPLACE INTO songs"
		" (album_id, folder_id, path, filename, title, track_number, disc_number,"
		"  year, genre, duration, bitrate, sample_rate, channels, codec,"
		"  file_size, file_modified, last_scanned)"
		" VALUES (?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,CURRENT_TIMESTAMP)");
	ins.bind(1,  album_id);
	ins.bind(2,  folder_id);
	ins.bind(3,  rel_path);
	ins.bind(4,  path.filename().string());
	ins.bind(5,  title);
	ins.bind(6,  track_nr);
	ins.bind(7,  disc_number > 0 ? disc_number : 1);
	ins.bind(8,  year);
	ins.bind(9,  genre);
	ins.bind(10, duration);
	ins.bind(11, bitrate);
	ins.bind(12, sr);
	ins.bind(13, channels);
	ins.bind(14, codec);
	ins.bind(15, file_size);
	ins.bind(16, mtime);
	ins.exec();

	// Link song to its artist (derived from the folder hierarchy).
	int song_id = static_cast<int>(db_music_.getLastInsertRowid());
	SQLite::Statement lnk(db_music_,
		"INSERT OR IGNORE INTO song_artists (song_id, artist_id, role)"
		" VALUES (?, ?, 'artist')");
	lnk.bind(1, song_id);
	lnk.bind(2, artist_id);
	lnk.exec();
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
	SQLite::Statement q(db_music_,
		"SELECT id, name FROM folders WHERE parent_id IS NULL");
	std::vector<MusicFolder> result;
	while (q.executeStep())
		result.push_back({ q.getColumn(0).getInt(),
		                   q.getColumn(1).getString() });
	return result;
	}

std::string MediaStore::get_cover_path(int folder_id)
	{
	std::lock_guard<std::mutex> lock(db_mutex_);
	SQLite::Statement q(db_music_,
		"SELECT cover_path FROM albums WHERE folder_id = ?");
	q.bind(1, folder_id);
	if (!q.executeStep() || q.getColumn(0).isNull()) return "";
	return q.getColumn(0).getString();
	}


std::vector<std::string> MediaStore::get_extra_image_paths(int folder_id)
	{
	std::string cover  = get_cover_path(folder_id);
	std::string folder = get_folder_path(folder_id);
	if (cover.empty() || folder.empty()) return {};
	// find_extra_images works in absolute paths (it walks the filesystem);
	// we strip back to relative on return so the public API stays uniform.
	auto abs_results = find_extra_images(abs_path(folder), abs_path(cover));
	std::vector<std::string> result;
	result.reserve(abs_results.size());
	for (auto& p : abs_results)
		result.push_back(strip_root(p, music_root_slash_));
	return result;
	}

int MediaStore::get_image_count(int folder_id)
	{
	std::string cover = get_cover_path(folder_id);
	if (cover.empty()) return 0;
	std::string folder = get_folder_path(folder_id);
	if (folder.empty()) return 0;
	return 1 + static_cast<int>(
		find_extra_images(abs_path(folder), abs_path(cover)).size());
	}

std::optional<MediaStore::SongInfo> MediaStore::get_song(int song_id)
	{
	std::lock_guard<std::mutex> lock(db_mutex_);
	SQLite::Statement q(db_music_,
		"SELECT id, path, codec, bitrate, duration, file_size"
		" FROM songs WHERE id = ?");
	q.bind(1, song_id);
	if (!q.executeStep()) return std::nullopt;
	SongInfo s;
	s.id        = q.getColumn(0).getInt();
	s.path      = q.getColumn(1).getString();
	s.codec     = q.getColumn(2).isNull() ? "" : q.getColumn(2).getString();
	s.bitrate   = q.getColumn(3).isNull() ? 0  : q.getColumn(3).getInt();
	s.duration  = q.getColumn(4).getDouble();
	s.file_size = q.getColumn(5).isNull() ? 0  : q.getColumn(5).getInt64();
	return s;
	}

std::vector<MediaStore::ArtistDir> MediaStore::get_artist_dirs()
	{
	std::lock_guard<std::mutex> lock(db_mutex_);
	// Count albums per artist via the child folders (album folders are one level down).
	SQLite::Statement sel(db_music_,
		"SELECT f.id, f.name, COUNT(al.id) AS album_count"
		" FROM folders f"
		" LEFT JOIN folders af ON af.parent_id = f.id"
		" LEFT JOIN albums al ON al.folder_id = af.id"
		" WHERE f.parent_id = (SELECT id FROM folders WHERE parent_id IS NULL)"
		" GROUP BY f.id"
		" ORDER BY f.name COLLATE NOCASE");

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
	SQLite::Statement fsel(db_music_,
		"SELECT f.id, f.name, f.parent_id,"
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

	bool flatten = flat_multi_disc && is_album;

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
	// subfolders (one level down), ordered by disc then track.
	const char* song_sql_flat =
		"SELECT s.id, s.title, s.track_number, s.disc_number,"
		"       s.year, s.genre, s.duration, s.bitrate,"
		"       s.file_size, s.codec, s.folder_id,"
		"       COALESCE(a.name, '') AS artist,"
		"       COALESCE(al.title, '') AS album"
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
		"       COALESCE(al.title, '') AS album"
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
		e.parent_id    = ssel.getColumn(10).getInt();  // actual folder (may be disc subfolder)
		e.artist       = ssel.getColumn(11).getString();
		e.album        = ssel.getColumn(12).getString();
		// Songs inherit cover art from their parent album folder.
		if (dir.cover_art_id >= 0)
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
	const std::string& username)
	{
	std::lock_guard<std::mutex> lock(db_mutex_);

	// Base SELECT — common to all types. The trailing column is a per-user
	// album-star flag, populated from a LEFT JOIN on client.stars.
	std::string sql =
		"SELECT f.id, COALESCE(f.parent_id,-1),"
		"       COALESCE(al.title, f.name),"
		"       COALESCE(a.name,''),"
		"       CASE WHEN al.cover_path IS NOT NULL AND al.cover_path != ''"
		"            THEN f.id ELSE -1 END,"
		"       COALESCE(al.song_count,0),"
		"       CAST(COALESCE(al.duration,0) AS INTEGER),"
		"       COALESCE(al.year,0),"
		"       COALESCE(al.genre,''),"
		"       COALESCE(al.created,''),"
		"       CASE WHEN sa.album_folder_path IS NOT NULL THEN 1 ELSE 0 END AS starred"
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
	auto add_where = [&](const char* clause) {
		sql += has_where ? " AND " : " WHERE ";
		sql += clause;
		has_where = true;
		};
	if      (type == "byYear")  add_where("al.year BETWEEN ? AND ?");
	else if (type == "byGenre") add_where("LOWER(COALESCE(al.genre,'')) = LOWER(?)");
	else if (type == "starred") add_where("sa.album_folder_path IS NOT NULL");

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
		e.starred      = q.getColumn(10).getInt() != 0;
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

	// Look up the artist folder itself.
	SQLite::Statement fsel(db_music_,
		"SELECT id, name FROM folders WHERE id = ?");
	fsel.bind(1, folder_id);
	if (!fsel.executeStep()) return std::nullopt;

	ArtistInfo info;
	info.artist.id   = fsel.getColumn(0).getInt();
	info.artist.name = fsel.getColumn(1).getString();

	// Fetch albums whose folder is a direct child of this artist folder.
	// Trailing column is the per-user album-star flag.
	SQLite::Statement asel(db_music_,
		"SELECT f.id, COALESCE(f.parent_id,-1),"
		"       COALESCE(al.title, f.name),"
		"       COALESCE(a.name,''),"
		"       CASE WHEN al.cover_path IS NOT NULL AND al.cover_path != ''"
		"            THEN f.id ELSE -1 END,"
		"       COALESCE(al.song_count,0),"
		"       CAST(COALESCE(al.duration,0) AS INTEGER),"
		"       COALESCE(al.year,0),"
		"       COALESCE(al.genre,''),"
		"       COALESCE(al.created,''),"
		"       CASE WHEN sa.album_folder_path IS NOT NULL THEN 1 ELSE 0 END AS starred"
		" FROM albums al"
		" JOIN folders f ON f.id = al.folder_id"
		" LEFT JOIN album_artists aa ON aa.album_id = al.id AND aa.role = 'albumartist'"
		" LEFT JOIN artists a ON a.id = aa.artist_id"
		" LEFT JOIN client.stars sa ON sa.album_folder_path = f.path"
		"      AND sa.user_id = (SELECT id FROM client.users WHERE username = ?)"
		" WHERE f.parent_id = ?"
		" ORDER BY al.year, al.title COLLATE NOCASE");
	asel.bind(1, username);
	asel.bind(2, folder_id);

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
		e.starred      = asel.getColumn(10).getInt() != 0;
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
		"SELECT f.id, COALESCE(f.parent_id,-1),"
		"       COALESCE(al.title, f.name),"
		"       COALESCE(a.name,''),"
		"       CASE WHEN al.cover_path IS NOT NULL AND al.cover_path != ''"
		"            THEN f.id ELSE -1 END,"
		"       COALESCE(al.song_count,0),"
		"       CAST(COALESCE(al.duration,0) AS INTEGER),"
		"       COALESCE(al.year,0),"
		"       COALESCE(al.genre,''),"
		"       COALESCE(al.created,''),"
		"       CASE WHEN sa.album_folder_path IS NOT NULL THEN 1 ELSE 0 END AS starred"
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
	info.album.starred      = msel.getColumn(10).getInt() != 0;

	// Fetch songs, flattening disc subfolders when flat_multi_disc is set.
	// The starred LEFT JOIN is added only when a username is provided.
	const std::string star_col  = username.empty()
		? ", 0 AS starred"
		: ", CASE WHEN st.song_path IS NOT NULL THEN 1 ELSE 0 END AS starred";
	const std::string star_join = username.empty()
		? ""
		: " LEFT JOIN client.stars st ON st.song_path = s.path"
		  " AND st.user_id = (SELECT id FROM client.users WHERE username = ?)";

	std::string song_sql_flat =
		"SELECT s.id, s.title, s.track_number, s.disc_number,"
		"       s.year, s.genre, s.duration, s.bitrate,"
		"       s.file_size, s.codec, s.folder_id,"
		"       COALESCE(a.name, '') AS artist,"
		"       COALESCE(al.title, '') AS album"
		+ star_col +
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
		" FROM songs s"
		" LEFT JOIN albums al ON al.id = s.album_id"
		" LEFT JOIN song_artists sa ON sa.song_id = s.id AND sa.role = 'artist'"
		" LEFT JOIN artists a ON a.id = sa.artist_id"
		+ star_join +
		" WHERE s.folder_id = ?"
		" ORDER BY s.disc_number, s.track_number, s.filename";

	SQLite::Statement ssel(db_music_, flat_multi_disc ? song_sql_flat : song_sql_normal);
	int idx = 1;
	if (!username.empty())
		ssel.bind(idx++, username);
	ssel.bind(idx++, folder_id);
	if (flat_multi_disc) ssel.bind(idx, folder_id);

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
		e.starred      = ssel.getColumn(13).getInt() != 0;
		if (info.album.cover_art_id >= 0)
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

	SQLite::Statement q(db_music_,
		"SELECT s.id, s.title, s.track_number, s.disc_number,"
		"       s.year, s.genre, s.duration, s.bitrate,"
		"       s.file_size, s.codec, s.folder_id,"
		"       COALESCE(a.name,'') AS artist,"
		"       COALESCE(al.title,'') AS album,"
		"       CASE WHEN al.cover_path IS NOT NULL AND al.cover_path != ''"
		"            THEN s.folder_id ELSE -1 END AS cover_art_id,"
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
	SQLite::Statement sq(db_music_,
		"SELECT s.id, s.title, s.track_number, s.disc_number,"
		"       s.year, s.genre, s.duration, s.bitrate,"
		"       s.file_size, s.codec, s.folder_id,"
		"       COALESCE(a.name,'') AS artist,"
		"       COALESCE(al.title,'') AS album,"
		"       CASE WHEN al.cover_path IS NOT NULL AND al.cover_path != ''"
		"            THEN s.folder_id ELSE -1 END AS cover_art_id"
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
		"SELECT f.id, COALESCE(f.parent_id,-1),"
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
	SQLite::Statement sq(db_music_,
		"SELECT s.id, s.title, s.track_number, s.disc_number,"
		"       s.year, s.genre, s.duration, s.bitrate,"
		"       s.file_size, s.codec, s.folder_id,"
		"       COALESCE(a.name,'') AS artist,"
		"       COALESCE(al.title,'') AS album,"
		"       CASE WHEN al.cover_path IS NOT NULL AND al.cover_path != ''"
		"            THEN s.folder_id ELSE -1 END AS cover_art_id"
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
	return join_root(rel, music_root_slash_);
	}

bool MediaStore::path_is_within_root(const fs::path& candidate) const
	{
	return is_within(candidate, music_root_canonical_);
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
	SQLite::Statement q(db_music_,
		"SELECT s.id, s.title, s.track_number, s.disc_number,"
		"       s.year, s.genre, s.duration, s.bitrate,"
		"       s.file_size, s.codec, s.folder_id,"
		"       COALESCE(a.name,'') AS artist,"
		"       COALESCE(al.title,'') AS album,"
		"       CASE WHEN al.cover_path IS NOT NULL AND al.cover_path != ''"
		"            THEN s.folder_id ELSE -1 END AS cover_art_id"
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
	return e;
	}

MediaStore::SearchResult MediaStore::search(const std::string& query,
                                             int artist_count, int artist_offset,
                                             int album_count,  int album_offset,
                                             int song_count,   int song_offset)
	{
	std::lock_guard<std::mutex> lock(db_mutex_);
	SearchResult result;
	std::string pattern = "%" + query + "%";

	// Artists — folder-level, depth-1 children of the root.
	SQLite::Statement aq(db_music_,
		"SELECT f.id, f.name"
		" FROM folders f"
		" WHERE f.parent_id = (SELECT id FROM folders WHERE parent_id IS NULL)"
		"   AND LOWER(f.name) LIKE LOWER(?)"
		" ORDER BY f.name COLLATE NOCASE"
		" LIMIT ? OFFSET ?");
	aq.bind(1, pattern);
	aq.bind(2, artist_count);
	aq.bind(3, artist_offset);
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
	SQLite::Statement alq(db_music_,
		"SELECT f.id, COALESCE(f.parent_id,-1),"
		"       COALESCE(al.title, f.name),"
		"       COALESCE(a.name,''),"
		"       CASE WHEN al.cover_path IS NOT NULL AND al.cover_path != ''"
		"            THEN f.id ELSE -1 END AS cover_art_id"
		" FROM albums al"
		" JOIN folders f ON f.id = al.folder_id"
		" LEFT JOIN album_artists aa ON aa.album_id = al.id AND aa.role = 'albumartist'"
		" LEFT JOIN artists a ON a.id = aa.artist_id"
		" WHERE LOWER(COALESCE(al.title, f.name)) LIKE LOWER(?)"
		" ORDER BY al.title COLLATE NOCASE"
		" LIMIT ? OFFSET ?");
	alq.bind(1, pattern);
	alq.bind(2, album_count);
	alq.bind(3, album_offset);
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
	SQLite::Statement sq(db_music_,
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
		" WHERE LOWER(s.title) LIKE LOWER(?)"
		" ORDER BY s.title COLLATE NOCASE"
		" LIMIT ? OFFSET ?");
	sq.bind(1, pattern);
	sq.bind(2, song_count);
	sq.bind(3, song_offset);
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
	SQLite::Statement q(db_music_,
		"SELECT s.id, s.title, s.track_number, s.disc_number,"
		"       s.year, s.genre, s.duration, s.bitrate,"
		"       s.file_size, s.codec, s.folder_id,"
		"       COALESCE(a.name,'') AS artist,"
		"       COALESCE(al.title,'') AS album,"
		"       CASE WHEN al.cover_path IS NOT NULL AND al.cover_path != ''"
		"            THEN s.folder_id ELSE -1 END AS cover_art_id,"
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
		? ", 0 AS starred"
		: ", CASE WHEN st.song_path IS NOT NULL THEN 1 ELSE 0 END AS starred";
	const std::string star_join = username.empty()
		? ""
		: " LEFT JOIN client.stars st ON st.song_path = s.path"
		  " AND st.user_id = (SELECT id FROM client.users WHERE username = ?)";

	std::string pl_sql =
		"SELECT s.id, s.title, s.track_number, s.disc_number,"
		"       s.year, s.genre, s.duration, s.bitrate,"
		"       s.file_size, s.codec, s.folder_id,"
		"       COALESCE(a.name,'') AS artist,"
		"       COALESCE(al.title,'') AS album,"
		"       CASE WHEN al.cover_path IS NOT NULL AND al.cover_path != ''"
		"            THEN s.folder_id ELSE -1 END AS cover_art_id"
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
		e.starred      = sq.getColumn(14).getInt() != 0;
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
