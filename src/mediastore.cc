#include "mediastore.hh"
#include "stamp.hh"
#include "md5.hh"

#include <iostream>
#include <algorithm>
#include <chrono>
#include <regex>
#include <set>

#include <taglib/fileref.h>
#include <tfilestream.h>
#include <taglib/tag.h>
#include <taglib/audioproperties.h>

namespace fs = std::filesystem;

static const std::set<std::string> AUDIO_EXTENSIONS = {
	".flac", ".mp3", ".ogg", ".m4a", ".aac", ".wav", ".opus", ".wma"
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
	// Pass 2: *front.jpg/jpeg  Pass 3: *.jpg/jpeg  (single directory scan for both).
	std::string jpg_fallback;
	for (auto& entry : fs::directory_iterator(dir)) {
		if (!entry.is_regular_file()) continue;
		std::string fname = entry.path().filename().string();
		if (iends_with(fname, "front.jpg") || iends_with(fname, "front.jpeg"))
			return entry.path().string();
		if (jpg_fallback.empty() && (iends_with(fname, ".jpg") || iends_with(fname, ".jpeg")))
			jpg_fallback = entry.path().string();
		}
	return jpg_fallback;
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
			path         TEXT NOT NULL UNIQUE,
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
			cover_path     TEXT,
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
			path                TEXT NOT NULL UNIQUE,
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
			folder_id   INTEGER PRIMARY KEY REFERENCES folders(id),
			mbid        TEXT NOT NULL DEFAULT '',
			last_fm_url TEXT NOT NULL DEFAULT '',
			biography   TEXT NOT NULL DEFAULT '',
			image_url   TEXT NOT NULL DEFAULT '',
			wiki_url    TEXT NOT NULL DEFAULT '',
			fetched_at  INTEGER NOT NULL DEFAULT (strftime('%s','now'))
		);

		CREATE TABLE IF NOT EXISTS album_info_cache (
			folder_id  INTEGER PRIMARY KEY REFERENCES folders(id),
			mbid       TEXT NOT NULL DEFAULT '',
			notes      TEXT NOT NULL DEFAULT '',
			wiki_url   TEXT NOT NULL DEFAULT '',
			fetched_at INTEGER NOT NULL DEFAULT (strftime('%s','now'))
		);
	)");

	// Client/user data tables (gaindrive-client.db, attached as "client" schema).
	db_music_.exec(R"(
		CREATE TABLE IF NOT EXISTS client.users (
			id           INTEGER PRIMARY KEY,
			username     TEXT NOT NULL UNIQUE,
			password_enc TEXT NOT NULL,
			email        TEXT,
			is_admin     INTEGER DEFAULT 0,
			max_bitrate  INTEGER DEFAULT 0,
			created      DATETIME DEFAULT CURRENT_TIMESTAMP,
			last_access  DATETIME
		);

		CREATE TABLE IF NOT EXISTS client.stars (
			user_id   INTEGER NOT NULL REFERENCES users(id)   ON DELETE CASCADE,
			song_id   INTEGER,
			album_id  INTEGER,
			artist_id INTEGER,
			created   DATETIME DEFAULT CURRENT_TIMESTAMP,
			UNIQUE(user_id, song_id, album_id, artist_id)
		);

		CREATE TABLE IF NOT EXISTS client.play_counts (
			user_id     INTEGER NOT NULL REFERENCES users(id) ON DELETE CASCADE,
			song_id     INTEGER NOT NULL,
			count       INTEGER DEFAULT 0,
			last_played DATETIME,
			PRIMARY KEY (user_id, song_id)
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
			song_id     INTEGER NOT NULL,
			position    INTEGER NOT NULL,
			PRIMARY KEY (playlist_id, position)
		);

		CREATE TABLE IF NOT EXISTS client.play_queue (
			user_id    INTEGER NOT NULL REFERENCES users(id) ON DELETE CASCADE,
			song_id    INTEGER NOT NULL,
			position   INTEGER NOT NULL,
			is_current INTEGER DEFAULT 0,
			offset_ms  INTEGER DEFAULT 0,
			client     TEXT,
			updated    DATETIME DEFAULT CURRENT_TIMESTAMP,
			PRIMARY KEY (user_id, position)
		);

		CREATE TABLE IF NOT EXISTS client.now_playing (
			user_id INTEGER NOT NULL REFERENCES users(id) ON DELETE CASCADE,
			song_id INTEGER NOT NULL,
			client  TEXT,
			started DATETIME DEFAULT CURRENT_TIMESTAMP,
			PRIMARY KEY (user_id)
		);

		CREATE TABLE IF NOT EXISTS client.bookmarks (
			user_id   INTEGER NOT NULL REFERENCES users(id) ON DELETE CASCADE,
			song_id   INTEGER NOT NULL,
			position  INTEGER NOT NULL DEFAULT 0,
			comment   TEXT,
			created   DATETIME DEFAULT CURRENT_TIMESTAMP,
			changed   DATETIME DEFAULT CURRENT_TIMESTAMP,
			PRIMARY KEY (user_id, song_id)
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

	// Backfill song_artists from album_artists for any songs that were scanned
	// before this link was introduced.
	db_music_.exec(
		"INSERT OR IGNORE INTO song_artists (song_id, artist_id, role)"
		" SELECT s.id, aa.artist_id, 'artist'"
		" FROM songs s"
		" JOIN album_artists aa ON aa.album_id = s.album_id AND aa.role = 'albumartist'"
		);
	}

// Returns a human-readable ETA string, e.g. "~3m20s" or "~45s".
static std::string format_eta(double remaining_sec)
	{
	int s = static_cast<int>(remaining_sec);
	if (s < 60) return "~" + std::to_string(s) + "s";
	return "~" + std::to_string(s / 60) + "m" + std::to_string(s % 60) + "s";
	}

MediaStore::Counts MediaStore::count_audio_files()
	{
	Counts c{};
	for (auto& a : fs::directory_iterator(music_root_)) {
		if (!a.is_directory()) continue;
		++c.artists;
		std::cout << stamp() << "(counting) " << a.path().filename().string() << std::endl;
		for (auto& b : fs::directory_iterator(a.path())) {
			if (!b.is_directory()) continue;
			++c.albums;
			for (auto& f : fs::directory_iterator(b.path())) {
				if (f.is_regular_file() && is_audio_file(f.path()))
					++c.files;
				else if (f.is_directory()) {
					// Disc subdirectory — count one level deeper, same as scan().
					for (auto& g : fs::directory_iterator(f.path()))
						if (g.is_regular_file() && is_audio_file(g.path())) ++c.files;
					}
				}
			}
		}
	return c;
	}

void MediaStore::scan()
	{
	Counts totals = count_audio_files();
	std::cout << stamp() << "Scan started: " << music_root_
	          << "  (" << totals.artists << " artists, "
	          << totals.albums << " albums, "
	          << totals.files  << " files)" << std::endl;

	using Clock = std::chrono::steady_clock;
	auto   start_time = Clock::now();
	int    song_count = 0;
	int    processed  = 0;

	std::string root_prefix = music_root_ + "/%";

	std::lock_guard<std::mutex> lock(db_mutex_);
	SQLite::Transaction txn(db_music_);

	// Mark every subfolder as unvisited.  upsert_folder will stamp each
	// directory it touches; anything still NULL after the walk no longer
	// exists on disk and will be pruned below.
	{
	SQLite::Statement s(db_music_,
		"UPDATE folders SET last_scanned = NULL WHERE path LIKE ?");
	s.bind(1, root_prefix);
	s.exec();
	}

	int root_id = upsert_folder(fs::path(music_root_), -1);

	for (auto& artist_entry : fs::directory_iterator(music_root_)) {
		if (!artist_entry.is_directory()) continue;
		int artist_folder_id = upsert_folder(artist_entry.path(), root_id);
		int artist_id        = upsert_artist(artist_entry.path().filename().string());
		std::cout << stamp() << artist_entry.path().filename().string() << std::endl;

		for (auto& album_entry : fs::directory_iterator(artist_entry.path())) {
			if (!album_entry.is_directory()) continue;
			int album_folder_id = upsert_folder(album_entry.path(), artist_folder_id);
			std::string album_title = album_entry.path().filename().string();
			std::replace(album_title.begin(), album_title.end(), '_', ' ');
			int album_id        = upsert_album(album_folder_id,
			                                   album_title,
			                                   artist_id, 0, "");

			// Store cover art path if found; don't clear an existing path on re-scan.
			std::string cover = find_cover(album_entry.path());
			std::cout << stamp() << "    cover: "
			          << (cover.empty() ? "(none)" : cover) << std::endl;
			if (!cover.empty()) {
				SQLite::Statement upd(db_music_,
					"UPDATE albums SET cover_path = ? WHERE id = ?");
				upd.bind(1, cover);
				upd.bind(2, album_id);
				upd.exec();
				}

			// Separate disc subdirs from audio files directly in the album dir.
			std::vector<fs::directory_entry> disc_dirs;
			std::vector<fs::path>            direct_files;
			for (auto& e : fs::directory_iterator(album_entry.path())) {
				if      (e.is_directory())                             disc_dirs.push_back(e);
				else if (e.is_regular_file() && is_audio_file(e.path())) direct_files.push_back(e.path());
				}
			std::sort(disc_dirs.begin(), disc_dirs.end(),
				[](const fs::directory_entry& a, const fs::directory_entry& b) {
					return a.path().filename() < b.path().filename();
					});

			// Disc subfolders: alphabetical order → disc numbers 1..N.
			int disc_count = (int)disc_dirs.size();
			for (int dn = 0; dn < disc_count; ++dn) {
				int disc_folder_id = upsert_folder(disc_dirs[dn].path(), album_folder_id);
				for (auto& te : fs::directory_iterator(disc_dirs[dn].path())) {
					if (!te.is_regular_file() || !is_audio_file(te.path())) continue;
					upsert_song(te.path(), album_id, disc_folder_id, artist_id, dn + 1);
					++song_count;
					++processed;
					}
				}

			// Audio files directly in the album dir (flat layout, or mixed).
			for (auto& p : direct_files) {
				upsert_song(p, album_id, album_folder_id, artist_id, 0);
				++song_count;
				++processed;
				}

			if (disc_count > 1) {
				SQLite::Statement upd(db_music_, "UPDATE albums SET disc_count=? WHERE id=?");
				upd.bind(1, disc_count);
				upd.bind(2, album_id);
				upd.exec();
				}

			// Derive album year from the first song that has one tagged.
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

			// Build progress suffix for this album's log line.
			std::string progress;
			if (processed == 0) {
				progress = "counting...";
				}
			else {
				double elapsed = std::chrono::duration<double>(Clock::now() - start_time).count();
				double rate    = processed / elapsed;
				double eta_sec = (totals.files - processed) / rate;
				progress = std::to_string(processed) + "/" + std::to_string(totals.files)
				         + " — ETA " + format_eta(eta_sec);
				}
			std::cout << stamp() << "  " << album_entry.path().filename().string()
			          << "  [" << progress << "]" << std::endl;
			}
		}

	// Prune entries for paths that no longer exist on disk.
	// Folders not visited during the walk were left with last_scanned = NULL
	// by the mark step above.  Delete their songs and albums first (no FK
	// cascade from folders → songs), then the folders themselves.
	{
	SQLite::Statement s(db_music_,
		"DELETE FROM songs WHERE folder_id IN ("
		"  SELECT id FROM folders WHERE last_scanned IS NULL AND path LIKE ?"
		")");
	s.bind(1, root_prefix);
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
	s.bind(1, root_prefix);
	s.exec();
	int n = db_music_.getChanges();
	if (n > 0)
		std::cout << stamp() << "  pruned " << n << " albums" << std::endl;
	}
	{
	SQLite::Statement s(db_music_,
		"DELETE FROM folders WHERE last_scanned IS NULL AND path LIKE ?");
	s.bind(1, root_prefix);
	s.exec();
	int n = db_music_.getChanges();
	if (n > 0)
		std::cout << stamp() << "  pruned " << n << " folders" << std::endl;
	}
	{
	// Artists are identified by name, not folder; prune any that have no albums left.
	SQLite::Statement s(db_music_,
		"DELETE FROM artists WHERE id NOT IN"
		" (SELECT DISTINCT artist_id FROM album_artists)");
	s.exec();
	int n = db_music_.getChanges();
	if (n > 0)
		std::cout << stamp() << "  pruned " << n << " artists" << std::endl;
	}

	txn.commit();
	std::cout << stamp() << "Scan complete: " << song_count << " songs" << std::endl;
	}

void MediaStore::scan_dirs(const std::set<std::string>& dirs)
	{
	// Queue overflow or other situation where we lost track of what changed.
	if (dirs.count(music_root_)) {
		scan();
		return;
		}
	for (auto& d : dirs)
		scan_artist_dir(fs::path(d));
	}

// Targeted rescan of one artist directory.  Same mark→walk→prune pattern as
// scan(), but scoped to a single artist subtree so most of the library is
// untouched.
void MediaStore::scan_artist_dir(const fs::path& artist_path)
	{
	std::string prefix = artist_path.string() + "/%";
	bool exists = fs::is_directory(artist_path);

	std::cout << stamp() << "Rescan: " << artist_path.filename().string()
	          << (exists ? "" : " (removed)") << std::endl;

	std::lock_guard<std::mutex> lock(db_mutex_);
	SQLite::Transaction txn(db_music_);

	// Mark this artist's subfolders as unvisited.
	{
	SQLite::Statement s(db_music_,
		"UPDATE folders SET last_scanned = NULL WHERE path LIKE ?");
	s.bind(1, prefix);
	s.exec();
	}

	if (exists) {
		int root_id          = upsert_folder(fs::path(music_root_), -1);
		int artist_folder_id = upsert_folder(artist_path, root_id);
		int artist_id        = upsert_artist(artist_path.filename().string());

		for (auto& album_entry : fs::directory_iterator(artist_path)) {
			if (!album_entry.is_directory()) continue;
			int album_folder_id = upsert_folder(album_entry.path(), artist_folder_id);
			std::string album_title = album_entry.path().filename().string();
			std::replace(album_title.begin(), album_title.end(), '_', ' ');
			int album_id = upsert_album(album_folder_id, album_title, artist_id, 0, "");

			std::string cover = find_cover(album_entry.path());
			if (!cover.empty()) {
				SQLite::Statement upd(db_music_,
					"UPDATE albums SET cover_path = ? WHERE id = ?");
				upd.bind(1, cover);
				upd.bind(2, album_id);
				upd.exec();
				}

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

			int disc_count = (int)disc_dirs.size();
			for (int dn = 0; dn < disc_count; ++dn) {
				int disc_folder_id = upsert_folder(disc_dirs[dn].path(), album_folder_id);
				for (auto& te : fs::directory_iterator(disc_dirs[dn].path())) {
					if (!te.is_regular_file() || !is_audio_file(te.path())) continue;
					upsert_song(te.path(), album_id, disc_folder_id, artist_id, dn + 1);
					}
				}
			for (auto& p : direct_files)
				upsert_song(p, album_id, album_folder_id, artist_id, 0);

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

			std::cout << stamp() << "  " << album_entry.path().filename().string() << std::endl;
			}
		}

	// Prune stale entries within this artist's subtree.
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

	txn.commit();
	std::cout << stamp() << "Rescan complete" << std::endl;
	}

// ---- upsert helpers ---------------------------------------------------

int MediaStore::upsert_folder(const fs::path& path, int parent_id)
	{
	std::string path_str = path.string();
	std::string name     = path.filename().string();
	if (name.empty()) name = path_str;  // for the root itself

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

	// Skip if the file hasn't changed since last scan.
	SQLite::Statement chk(db_music_,
		"SELECT file_modified FROM songs WHERE path = ?");
	chk.bind(1, path.string());
	if (chk.executeStep()) {
		if (chk.getColumn(0).getInt64() == mtime) {
			// Still update disc_number if it was folder-derived — folders may be reordered.
			if (disc_number > 0) {
				SQLite::Statement upd(db_music_,
					"UPDATE songs SET disc_number=? WHERE path=? AND disc_number!=?");
				upd.bind(1, disc_number);
				upd.bind(2, path.string());
				upd.bind(3, disc_number);
				upd.exec();
				}
			return;
			}
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
	ins.bind(3,  path.string());
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
		"SELECT username, email, is_admin FROM client.users WHERE username = ?");
	sel.bind(1, username);
	if (!sel.executeStep()) return std::nullopt;
	UserInfo u;
	u.username = sel.getColumn(0).getString();
	u.email    = sel.getColumn(1).isNull() ? "" : sel.getColumn(1).getString();
	u.is_admin = sel.getColumn(2).getInt() != 0;
	return u;
	}

bool MediaStore::validate_auth(const std::string& username,
                                const std::string& password,
                                const std::string& token,
                                const std::string& salt)
	{
	std::lock_guard<std::mutex> lock(db_mutex_);
	SQLite::Statement sel(db_music_,
		"SELECT password_enc FROM client.users WHERE username = ?");
	sel.bind(1, username);
	if (!sel.executeStep()) return false;
	std::string stored = sel.getColumn(0).getString();

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
		"SELECT mbid, last_fm_url, biography, image_url, wiki_url"
		" FROM artist_info_cache WHERE folder_id = ?");
	q.bind(1, folder_id);
	if (!q.executeStep()) return std::nullopt;
	CachedArtistInfo a;
	a.mbid       = q.getColumn(0).getString();
	a.last_fm_url= q.getColumn(1).getString();
	a.biography  = q.getColumn(2).getString();
	a.image_url  = q.getColumn(3).getString();
	a.wiki_url   = q.getColumn(4).getString();
	return a;
	}

void MediaStore::cache_artist_info(int folder_id, const CachedArtistInfo& info)
	{
	std::lock_guard<std::mutex> lock(db_mutex_);
	SQLite::Statement ins(db_music_,
		"INSERT OR REPLACE INTO artist_info_cache"
		" (folder_id, mbid, last_fm_url, biography, image_url, wiki_url)"
		" VALUES (?, ?, ?, ?, ?, ?)");
	ins.bind(1, folder_id);
	ins.bind(2, info.mbid);
	ins.bind(3, info.last_fm_url);
	ins.bind(4, info.biography);
	ins.bind(5, info.image_url);
	ins.bind(6, info.wiki_url);
	ins.exec();
	}

std::optional<MediaStore::CachedAlbumInfo>
MediaStore::get_cached_album_info(int folder_id)
	{
	std::lock_guard<std::mutex> lock(db_mutex_);
	SQLite::Statement q(db_music_,
		"SELECT mbid, notes, wiki_url FROM album_info_cache WHERE folder_id = ?");
	q.bind(1, folder_id);
	if (!q.executeStep()) return std::nullopt;
	CachedAlbumInfo a;
	a.mbid     = q.getColumn(0).getString();
	a.notes    = q.getColumn(1).getString();
	a.wiki_url = q.getColumn(2).getString();
	return a;
	}

void MediaStore::cache_album_info(int folder_id, const CachedAlbumInfo& info)
	{
	std::lock_guard<std::mutex> lock(db_mutex_);
	SQLite::Statement ins(db_music_,
		"INSERT OR REPLACE INTO album_info_cache"
		" (folder_id, mbid, notes, wiki_url, fetched_at)"
		" VALUES (?, ?, ?, ?, strftime('%s','now'))");
	ins.bind(1, folder_id);
	ins.bind(2, info.mbid);
	ins.bind(3, info.notes);
	ins.bind(4, info.wiki_url);
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

	// Base SELECT — common to all types.
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
		"       COALESCE(al.created,'')"
		" FROM albums al"
		" JOIN folders f ON f.id = al.folder_id"
		" LEFT JOIN album_artists aa ON aa.album_id = al.id AND aa.role = 'albumartist'"
		" LEFT JOIN artists a ON a.id = aa.artist_id";

	// Extra joins for play-count-based types.
	bool play_count_join = (type == "frequent" || type == "recent");
	if (play_count_join)
		sql += " LEFT JOIN songs s ON s.album_id = al.id"
		       " LEFT JOIN client.play_counts pc ON pc.song_id = s.id"
		       " AND pc.user_id = (SELECT id FROM client.users WHERE username = ?)";

	// Join for starred.
	if (type == "starred")
		sql += " JOIN client.stars st ON st.album_id = f.id"
		       " JOIN client.users u ON u.id = st.user_id AND u.username = ?";

	// WHERE clause.
	if      (type == "byYear")  sql += " WHERE al.year BETWEEN ? AND ?";
	else if (type == "byGenre") sql += " WHERE LOWER(COALESCE(al.genre,'')) = LOWER(?)";

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
	else if (type == "starred")             sql += " ORDER BY st.created DESC";
	else if (type == "byYear")              sql += " ORDER BY al.year";
	else if (type == "byGenre")             sql += " ORDER BY al.title COLLATE NOCASE";
	else                                    sql += " ORDER BY al.created DESC"; // fallback

	sql += " LIMIT ? OFFSET ?";

	SQLite::Statement q(db_music_, sql);
	int idx = 1;

	if (play_count_join)  q.bind(idx++, username);
	if (type == "starred") q.bind(idx++, username);
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
		result.push_back(std::move(e));
		}
	return result;
	}

std::optional<MediaStore::ArtistInfo> MediaStore::get_artist(int folder_id)
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
		"       COALESCE(al.created,'')"
		" FROM albums al"
		" JOIN folders f ON f.id = al.folder_id"
		" LEFT JOIN album_artists aa ON aa.album_id = al.id AND aa.role = 'albumartist'"
		" LEFT JOIN artists a ON a.id = aa.artist_id"
		" WHERE f.parent_id = ?"
		" ORDER BY al.year, al.title COLLATE NOCASE");
	asel.bind(1, folder_id);

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
		info.albums.push_back(std::move(e));
		}

	info.artist.album_count = (int)info.albums.size();
	return info;
	}

std::optional<MediaStore::AlbumInfo> MediaStore::get_album(int folder_id,
                                                             bool flat_multi_disc)
	{
	std::lock_guard<std::mutex> lock(db_mutex_);

	// Fetch album metadata.
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
		"       COALESCE(al.created,'')"
		" FROM albums al"
		" JOIN folders f ON f.id = al.folder_id"
		" LEFT JOIN album_artists aa ON aa.album_id = al.id AND aa.role = 'albumartist'"
		" LEFT JOIN artists a ON a.id = aa.artist_id"
		" WHERE f.id = ?");
	msel.bind(1, folder_id);
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

	// Fetch songs, flattening disc subfolders when flat_multi_disc is set.
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

	SQLite::Statement ssel(db_music_, flat_multi_disc ? song_sql_flat : song_sql_normal);
	ssel.bind(1, folder_id);
	if (flat_multi_disc) ssel.bind(2, folder_id);

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
		e.parent_id    = ssel.getColumn(10).getInt();
		e.artist       = ssel.getColumn(11).getString();
		e.album        = ssel.getColumn(12).getString();
		if (info.album.cover_art_id >= 0)
			e.cover_art_id = info.album.cover_art_id;
		info.songs.push_back(std::move(e));
		}

	return info;
	}

// ---- Play queue / bookmarks ------------------------------------------

void MediaStore::save_play_queue(const std::string& username,
                                  const std::vector<int>& song_ids,
                                  int current_id, int64_t offset_ms,
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
		"INSERT INTO client.play_queue (user_id, song_id, position, is_current, offset_ms, client)"
		" VALUES (?, ?, ?, ?, ?, ?)");
	for (int pos = 0; pos < static_cast<int>(song_ids.size()); ++pos) {
		int  sid     = song_ids[pos];
		bool is_curr = (sid == current_id);
		ins.bind(1, user_id);
		ins.bind(2, sid);
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
		" JOIN songs s ON s.id = pq.song_id"
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
                                  int song_id, int64_t position_ms,
                                  const std::string& comment)
	{
	std::lock_guard<std::mutex> lock(db_mutex_);

	SQLite::Statement uid_q(db_music_, "SELECT id FROM client.users WHERE username = ?");
	uid_q.bind(1, username);
	if (!uid_q.executeStep()) return;
	int user_id = uid_q.getColumn(0).getInt();

	SQLite::Statement ins(db_music_,
		"INSERT INTO client.bookmarks (user_id, song_id, position, comment, changed)"
		" VALUES (?, ?, ?, ?, CURRENT_TIMESTAMP)"
		" ON CONFLICT(user_id, song_id) DO UPDATE SET"
		"   position = excluded.position,"
		"   comment  = excluded.comment,"
		"   changed  = CURRENT_TIMESTAMP");
	ins.bind(1, user_id);
	ins.bind(2, song_id);
	ins.bind(3, position_ms);
	ins.bind(4, comment);
	ins.exec();
	}

void MediaStore::scrobble(const std::string& username, int song_id,
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
			"INSERT INTO client.play_counts (user_id, song_id, count, last_played)"
			" VALUES (?, ?, 1, CURRENT_TIMESTAMP)"
			" ON CONFLICT(user_id, song_id) DO UPDATE SET"
			"   count       = count + 1,"
			"   last_played = CURRENT_TIMESTAMP");
		ins.bind(1, user_id);
		ins.bind(2, song_id);
		ins.exec();
		} else {
		// Now-playing notification — update or replace the single row.
		SQLite::Statement ins(db_music_,
			"INSERT OR REPLACE INTO client.now_playing (user_id, song_id, client, started)"
			" VALUES (?, ?, ?, CURRENT_TIMESTAMP)");
		ins.bind(1, user_id);
		ins.bind(2, song_id);
		ins.bind(3, client);
		ins.exec();
		}
	}

void MediaStore::add_star(const std::string& username,
                          int song_id, int album_id, int artist_id)
	{
	std::lock_guard<std::mutex> lock(db_mutex_);
	SQLite::Statement uid_q(db_music_, "SELECT id FROM client.users WHERE username = ?");
	uid_q.bind(1, username);
	if (!uid_q.executeStep()) return;
	int user_id = uid_q.getColumn(0).getInt();

	SQLite::Statement ins(db_music_,
		"INSERT OR IGNORE INTO client.stars (user_id, song_id, album_id, artist_id)"
		" VALUES (?, NULLIF(?,0), NULLIF(?,0), NULLIF(?,0))");
	ins.bind(1, user_id);
	ins.bind(2, song_id);
	ins.bind(3, album_id);
	ins.bind(4, artist_id);
	ins.exec();
	}

void MediaStore::remove_star(const std::string& username,
                             int song_id, int album_id, int artist_id)
	{
	std::lock_guard<std::mutex> lock(db_mutex_);
	SQLite::Statement uid_q(db_music_, "SELECT id FROM client.users WHERE username = ?");
	uid_q.bind(1, username);
	if (!uid_q.executeStep()) return;
	int user_id = uid_q.getColumn(0).getInt();

	SQLite::Statement del(db_music_,
		"DELETE FROM client.stars WHERE user_id=?"
		" AND (song_id IS NULLIF(?,0))"
		" AND (album_id IS NULLIF(?,0))"
		" AND (artist_id IS NULLIF(?,0))");
	del.bind(1, user_id);
	del.bind(2, song_id);
	del.bind(3, album_id);
	del.bind(4, artist_id);
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
		" JOIN songs s ON s.id = st.song_id"
		" LEFT JOIN albums al ON al.id = s.album_id"
		" LEFT JOIN song_artists sa ON sa.song_id = s.id AND sa.role = 'artist'"
		" LEFT JOIN artists a ON a.id = sa.artist_id"
		" WHERE u.username = ? AND st.song_id IS NOT NULL"
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

	// Starred albums — stars.album_id stores the folder id of the album dir.
	SQLite::Statement aq(db_music_,
		"SELECT f.id, COALESCE(f.parent_id,-1),"
		"       COALESCE(al.title, f.name) AS title,"
		"       COALESCE(a.name,'') AS artist,"
		"       CASE WHEN al.cover_path IS NOT NULL AND al.cover_path != ''"
		"            THEN f.id ELSE -1 END AS cover_art_id"
		" FROM client.stars st"
		" JOIN client.users u ON u.id = st.user_id"
		" JOIN folders f ON f.id = st.album_id"
		" LEFT JOIN albums al ON al.folder_id = f.id"
		" LEFT JOIN album_artists aa ON aa.album_id = al.id AND aa.role = 'albumartist'"
		" LEFT JOIN artists a ON a.id = aa.artist_id"
		" WHERE u.username = ? AND st.album_id IS NOT NULL"
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

	// Starred artists — stars.artist_id stores the folder id of the artist dir.
	SQLite::Statement arq(db_music_,
		"SELECT f.id, f.name"
		" FROM client.stars st"
		" JOIN client.users u ON u.id = st.user_id"
		" JOIN folders f ON f.id = st.artist_id"
		" WHERE u.username = ? AND st.artist_id IS NOT NULL"
		" ORDER BY st.created DESC");
	arq.bind(1, username);
	while (arq.executeStep())
		result.artists.push_back({arq.getColumn(0).getInt(),
		                          arq.getColumn(1).getString()});

	return result;
	}

MediaStore::PlaylistInfo MediaStore::create_playlist(const std::string& username,
                                                      const std::string& name,
                                                      const std::vector<int>& song_ids)
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
		"INSERT INTO client.playlist_songs (playlist_id, song_id, position) VALUES (?, ?, ?)");
	for (int pos = 0; pos < (int)song_ids.size(); ++pos) {
		sins.bind(1, playlist_id);
		sins.bind(2, song_ids[pos]);
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
		" JOIN songs s ON s.id = ps.song_id"
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

	// Songs.
	SQLite::Statement sq(db_music_,
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
		" JOIN songs s ON s.id = b.song_id"
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

bool MediaStore::delete_bookmark(const std::string& username, int song_id)
	{
	std::lock_guard<std::mutex> lock(db_mutex_);
	SQLite::Statement del(db_music_,
		"DELETE FROM client.bookmarks WHERE song_id = ?"
		" AND user_id = (SELECT id FROM client.users WHERE username = ?)");
	del.bind(1, song_id);
	del.bind(2, username);
	del.exec();
	return db_music_.getChanges() > 0;
	}

bool MediaStore::update_playlist(int playlist_id, const std::string& username,
                                  const std::optional<std::string>& name,
                                  const std::optional<std::string>& comment,
                                  const std::optional<bool>& is_public,
                                  const std::vector<int>& songs_to_add,
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

	// Read surviving song_ids in position order, then drop requested indices.
	SQLite::Statement sel(db_music_,
		"SELECT song_id FROM client.playlist_songs WHERE playlist_id = ? ORDER BY position");
	sel.bind(1, playlist_id);
	std::vector<int> kept;
	while (sel.executeStep())
		kept.push_back(sel.getColumn(0).getInt());

	// Remove in descending index order to avoid shifting.
	std::vector<int> sorted_remove = indices_to_remove;
	std::sort(sorted_remove.rbegin(), sorted_remove.rend());
	for (int idx : sorted_remove)
		if (idx >= 0 && idx < (int)kept.size())
			kept.erase(kept.begin() + idx);

	for (int id : songs_to_add)
		kept.push_back(id);

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
		"INSERT INTO client.playlist_songs (playlist_id, song_id, position) VALUES (?,?,?)");
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
		"       COUNT(ps.song_id), COALESCE(SUM(s.duration),0),"
		"       p.created, p.updated"
		" FROM client.playlists p"
		" JOIN client.users u ON u.id = p.user_id"
		" LEFT JOIN client.playlist_songs ps ON ps.playlist_id = p.id"
		" LEFT JOIN songs s ON s.id = ps.song_id"
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

std::optional<MediaStore::PlaylistInfo> MediaStore::get_playlist(int playlist_id)
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

	SQLite::Statement sq(db_music_,
		"SELECT s.id, s.title, s.track_number, s.disc_number,"
		"       s.year, s.genre, s.duration, s.bitrate,"
		"       s.file_size, s.codec, s.folder_id,"
		"       COALESCE(a.name,'') AS artist,"
		"       COALESCE(al.title,'') AS album,"
		"       CASE WHEN al.cover_path IS NOT NULL AND al.cover_path != ''"
		"            THEN s.folder_id ELSE -1 END AS cover_art_id"
		" FROM client.playlist_songs ps"
		" JOIN songs s ON s.id = ps.song_id"
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
