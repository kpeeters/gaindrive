#include "mediastore.hh"
#include "stamp.hh"

#include <iostream>
#include <algorithm>
#include <chrono>
#include <set>

#include <taglib/fileref.h>
#include <tfilestream.h>
#include <taglib/tag.h>
#include <taglib/audioproperties.h>

namespace fs = std::filesystem;

static const std::set<std::string> AUDIO_EXTENSIONS = {
	".flac", ".mp3", ".ogg", ".m4a", ".aac", ".wav", ".opus", ".wma"
	};

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

MediaStore::MediaStore(const std::string& db_path, const std::string& music_root)
	: music_root_(music_root),
	  db_(db_path, SQLite::OPEN_READWRITE | SQLite::OPEN_CREATE)
	{
	db_.exec("PRAGMA journal_mode=WAL");
	db_.exec("PRAGMA foreign_keys=ON");
	create_schema();
	}

void MediaStore::create_schema()
	{
	SQLite::Transaction txn(db_);
	db_.exec(R"(
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
			folder_id      INTEGER NOT NULL REFERENCES folders(id),
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

		CREATE TABLE IF NOT EXISTS users (
			id           INTEGER PRIMARY KEY,
			username     TEXT NOT NULL UNIQUE,
			password_enc TEXT NOT NULL,
			email        TEXT,
			is_admin     INTEGER DEFAULT 0,
			max_bitrate  INTEGER DEFAULT 0,
			created      DATETIME DEFAULT CURRENT_TIMESTAMP,
			last_access  DATETIME
		);

		CREATE TABLE IF NOT EXISTS stars (
			user_id   INTEGER NOT NULL REFERENCES users(id)   ON DELETE CASCADE,
			song_id   INTEGER          REFERENCES songs(id)   ON DELETE CASCADE,
			album_id  INTEGER          REFERENCES albums(id)  ON DELETE CASCADE,
			artist_id INTEGER          REFERENCES artists(id) ON DELETE CASCADE,
			created   DATETIME DEFAULT CURRENT_TIMESTAMP,
			UNIQUE(user_id, song_id, album_id, artist_id)
		);

		CREATE TABLE IF NOT EXISTS play_counts (
			user_id     INTEGER NOT NULL REFERENCES users(id) ON DELETE CASCADE,
			song_id     INTEGER NOT NULL REFERENCES songs(id) ON DELETE CASCADE,
			count       INTEGER DEFAULT 0,
			last_played DATETIME,
			PRIMARY KEY (user_id, song_id)
		);

		CREATE TABLE IF NOT EXISTS playlists (
			id        INTEGER PRIMARY KEY,
			user_id   INTEGER NOT NULL REFERENCES users(id) ON DELETE CASCADE,
			name      TEXT NOT NULL,
			comment   TEXT,
			is_public INTEGER DEFAULT 0,
			created   DATETIME DEFAULT CURRENT_TIMESTAMP,
			updated   DATETIME DEFAULT CURRENT_TIMESTAMP
		);

		CREATE TABLE IF NOT EXISTS playlist_songs (
			playlist_id INTEGER NOT NULL REFERENCES playlists(id) ON DELETE CASCADE,
			song_id     INTEGER NOT NULL REFERENCES songs(id)     ON DELETE CASCADE,
			position    INTEGER NOT NULL,
			PRIMARY KEY (playlist_id, position)
		);

		CREATE TABLE IF NOT EXISTS play_queue (
			user_id    INTEGER NOT NULL REFERENCES users(id) ON DELETE CASCADE,
			song_id    INTEGER NOT NULL REFERENCES songs(id) ON DELETE CASCADE,
			position   INTEGER NOT NULL,
			is_current INTEGER DEFAULT 0,
			offset_ms  INTEGER DEFAULT 0,
			client     TEXT,
			updated    DATETIME DEFAULT CURRENT_TIMESTAMP,
			PRIMARY KEY (user_id, position)
		);

		CREATE TABLE IF NOT EXISTS now_playing (
			user_id INTEGER NOT NULL REFERENCES users(id) ON DELETE CASCADE,
			song_id INTEGER NOT NULL REFERENCES songs(id),
			client  TEXT,
			started DATETIME DEFAULT CURRENT_TIMESTAMP,
			PRIMARY KEY (user_id)
		);
	)");
	txn.commit();
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
				if (f.is_regular_file() && is_audio_file(f.path())) ++c.files;
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

	std::lock_guard<std::mutex> lock(db_mutex_);
	SQLite::Transaction txn(db_);

	int root_id = upsert_folder(fs::path(music_root_), -1);

	for (auto& artist_entry : fs::directory_iterator(music_root_)) {
		if (!artist_entry.is_directory()) continue;
		int artist_folder_id = upsert_folder(artist_entry.path(), root_id);
		int artist_id        = upsert_artist(artist_entry.path().filename().string());
		std::cout << stamp() << artist_entry.path().filename().string() << std::endl;

		for (auto& album_entry : fs::directory_iterator(artist_entry.path())) {
			if (!album_entry.is_directory()) continue;
			int album_folder_id = upsert_folder(album_entry.path(), artist_folder_id);
			int album_id        = upsert_album(album_folder_id,
			                                   album_entry.path().filename().string(),
			                                   artist_id, 0, "");

			for (auto& track_entry : fs::directory_iterator(album_entry.path())) {
				if (!track_entry.is_regular_file()) continue;
				if (!is_audio_file(track_entry.path())) continue;
				upsert_song(track_entry.path(), album_id, album_folder_id);
				++song_count;
				++processed;
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

	txn.commit();
	std::cout << stamp() << "Scan complete: " << song_count << " songs" << std::endl;
	}

// ---- upsert helpers ---------------------------------------------------

int MediaStore::upsert_folder(const fs::path& path, int parent_id)
	{
	std::string path_str = path.string();
	std::string name     = path.filename().string();
	if (name.empty()) name = path_str;  // for the root itself

	if (parent_id < 0) {
		SQLite::Statement ins(db_,
			"INSERT OR IGNORE INTO folders (path, name, last_scanned)"
			" VALUES (?, ?, CURRENT_TIMESTAMP)");
		ins.bind(1, path_str);
		ins.bind(2, name);
		ins.exec();
		}
	else {
		SQLite::Statement ins(db_,
			"INSERT OR IGNORE INTO folders (path, name, parent_id, last_scanned)"
			" VALUES (?, ?, ?, CURRENT_TIMESTAMP)");
		ins.bind(1, path_str);
		ins.bind(2, name);
		ins.bind(3, parent_id);
		ins.exec();
		}

	SQLite::Statement upd(db_,
		"UPDATE folders SET last_scanned = CURRENT_TIMESTAMP WHERE path = ?");
	upd.bind(1, path_str);
	upd.exec();

	SQLite::Statement sel(db_, "SELECT id FROM folders WHERE path = ?");
	sel.bind(1, path_str);
	sel.executeStep();
	return sel.getColumn(0).getInt();
	}

int MediaStore::upsert_artist(const std::string& name)
	{
	SQLite::Statement ins(db_,
		"INSERT OR IGNORE INTO artists (name) VALUES (?)");
	ins.bind(1, name);
	ins.exec();

	SQLite::Statement sel(db_, "SELECT id FROM artists WHERE name = ?");
	sel.bind(1, name);
	sel.executeStep();
	return sel.getColumn(0).getInt();
	}

int MediaStore::upsert_album(int folder_id, const std::string& title,
                              int artist_id, int year, const std::string& genre)
	{
	{
	SQLite::Statement ins(db_,
		"INSERT OR IGNORE INTO albums (folder_id, title, year, genre, last_scanned)"
		" VALUES (?, ?, ?, ?, CURRENT_TIMESTAMP)");
	ins.bind(1, folder_id);
	ins.bind(2, title);
	ins.bind(3, year);
	ins.bind(4, genre);
	ins.exec();
	}

	SQLite::Statement sel(db_, "SELECT id FROM albums WHERE folder_id = ?");
	sel.bind(1, folder_id);
	sel.executeStep();
	int album_id = sel.getColumn(0).getInt();

	// Link album to its artist (by folder convention: the artist dir above).
	SQLite::Statement lnk(db_,
		"INSERT OR IGNORE INTO album_artists (album_id, artist_id, role)"
		" VALUES (?, ?, 'albumartist')");
	lnk.bind(1, album_id);
	lnk.bind(2, artist_id);
	lnk.exec();

	return album_id;
	}

void MediaStore::upsert_song(const fs::path& path, int album_id, int folder_id)
	{
	int64_t mtime = mtime_of(path);

	// Skip if the file hasn't changed since last scan.
	SQLite::Statement chk(db_,
		"SELECT file_modified FROM songs WHERE path = ?");
	chk.bind(1, path.string());
	if (chk.executeStep()) {
		if (chk.getColumn(0).getInt64() == mtime)
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

	SQLite::Statement ins(db_,
		"INSERT OR REPLACE INTO songs"
		" (album_id, folder_id, path, filename, title, track_number,"
		"  year, genre, duration, bitrate, sample_rate, channels, codec,"
		"  file_size, file_modified, last_scanned)"
		" VALUES (?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,CURRENT_TIMESTAMP)");
	ins.bind(1,  album_id);
	ins.bind(2,  folder_id);
	ins.bind(3,  path.string());
	ins.bind(4,  path.filename().string());
	ins.bind(5,  title);
	ins.bind(6,  track_nr);
	ins.bind(7,  year);
	ins.bind(8,  genre);
	ins.bind(9,  duration);
	ins.bind(10, bitrate);
	ins.bind(11, sr);
	ins.bind(12, channels);
	ins.bind(13, codec);
	ins.bind(14, file_size);
	ins.bind(15, mtime);
	ins.exec();
	}
