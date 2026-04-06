#include "mediastore.hh"
#include "stamp.hh"
#include "md5.hh"

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

// Candidate cover art filenames in priority order.  Add more here as needed.
static const std::vector<std::string> COVER_FILENAMES = {
	"cover.jpg", "folder.jpg", "front.jpg", "cover.png"
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
	// Pass 2: *front.jpg  Pass 3: *.jpg  (single directory scan for both).
	std::string jpg_fallback;
	for (auto& entry : fs::directory_iterator(dir)) {
		if (!entry.is_regular_file()) continue;
		std::string fname = entry.path().filename().string();
		if (iends_with(fname, "front.jpg")) return entry.path().string();
		if (jpg_fallback.empty() && iends_with(fname, ".jpg"))
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

		CREATE TABLE IF NOT EXISTS bookmarks (
			user_id   INTEGER NOT NULL REFERENCES users(id) ON DELETE CASCADE,
			song_id   INTEGER NOT NULL REFERENCES songs(id) ON DELETE CASCADE,
			position  INTEGER NOT NULL DEFAULT 0,
			comment   TEXT,
			created   DATETIME DEFAULT CURRENT_TIMESTAMP,
			changed   DATETIME DEFAULT CURRENT_TIMESTAMP,
			PRIMARY KEY (user_id, song_id)
		);

		CREATE TABLE IF NOT EXISTS artist_info_cache (
			folder_id   INTEGER PRIMARY KEY REFERENCES folders(id),
			mbid        TEXT NOT NULL DEFAULT '',
			last_fm_url TEXT NOT NULL DEFAULT '',
			biography   TEXT NOT NULL DEFAULT '',
			image_url   TEXT NOT NULL DEFAULT '',
			fetched_at  INTEGER NOT NULL DEFAULT (strftime('%s','now'))
		);
	)");
	txn.commit();

	// Migrations for existing databases: add columns if they don't exist yet.
	try { db_.exec("ALTER TABLE artist_info_cache ADD COLUMN biography TEXT NOT NULL DEFAULT ''"); }
	catch (const SQLite::Exception&) {}
	try { db_.exec("ALTER TABLE artist_info_cache ADD COLUMN image_url TEXT NOT NULL DEFAULT ''"); }
	catch (const SQLite::Exception&) {}

	// Backfill song_artists from album_artists for any songs that were scanned
	// before this link was introduced.
	db_.exec(
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

			// Store cover art path if found; don't clear an existing path on re-scan.
			std::string cover = find_cover(album_entry.path());
			std::cout << stamp() << "    cover: "
			          << (cover.empty() ? "(none)" : cover) << std::endl;
			if (!cover.empty()) {
				SQLite::Statement upd(db_,
					"UPDATE albums SET cover_path = ? WHERE id = ?");
				upd.bind(1, cover);
				upd.bind(2, album_id);
				upd.exec();
				}

			for (auto& track_entry : fs::directory_iterator(album_entry.path())) {
				if (!track_entry.is_regular_file()) continue;
				if (!is_audio_file(track_entry.path())) continue;
				upsert_song(track_entry.path(), album_id, album_folder_id, artist_id);
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

void MediaStore::upsert_song(const fs::path& path, int album_id, int folder_id,
                              int artist_id)
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

	// Link song to its artist (derived from the folder hierarchy).
	int song_id = static_cast<int>(db_.getLastInsertRowid());
	SQLite::Statement lnk(db_,
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
	SQLite::Statement ins(db_,
		"INSERT OR IGNORE INTO users (username, password_enc, is_admin) VALUES (?,?,?)");
	ins.bind(1, username);
	ins.bind(2, password);
	ins.bind(3, is_admin ? 1 : 0);
	ins.exec();
	return db_.getChanges() > 0;
	}

bool MediaStore::has_users()
	{
	std::lock_guard<std::mutex> lock(db_mutex_);
	SQLite::Statement sel(db_, "SELECT COUNT(*) FROM users");
	sel.executeStep();
	return sel.getColumn(0).getInt() > 0;
	}

std::optional<MediaStore::UserInfo> MediaStore::get_user(const std::string& username)
	{
	std::lock_guard<std::mutex> lock(db_mutex_);
	SQLite::Statement sel(db_,
		"SELECT username, email, is_admin FROM users WHERE username = ?");
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
	SQLite::Statement sel(db_,
		"SELECT password_enc FROM users WHERE username = ?");
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
		SQLite::Statement upd(db_,
			"UPDATE users SET last_access = CURRENT_TIMESTAMP WHERE username = ?");
		upd.bind(1, username);
		upd.exec();
		}

	return ok;
	}

// ---- Library browsing ------------------------------------------------

std::string MediaStore::get_folder_name(int folder_id)
	{
	std::lock_guard<std::mutex> lock(db_mutex_);
	SQLite::Statement q(db_, "SELECT name FROM folders WHERE id = ?");
	q.bind(1, folder_id);
	return q.executeStep() ? q.getColumn(0).getString() : "";
	}

std::optional<MediaStore::CachedArtistInfo> MediaStore::get_cached_artist_info(int folder_id)
	{
	std::lock_guard<std::mutex> lock(db_mutex_);
	SQLite::Statement q(db_,
		"SELECT mbid, last_fm_url, biography, image_url"
		" FROM artist_info_cache WHERE folder_id = ?");
	q.bind(1, folder_id);
	if (!q.executeStep()) return std::nullopt;
	CachedArtistInfo a;
	a.mbid       = q.getColumn(0).getString();
	a.last_fm_url= q.getColumn(1).getString();
	a.biography  = q.getColumn(2).getString();
	a.image_url  = q.getColumn(3).getString();
	return a;
	}

void MediaStore::cache_artist_info(int folder_id, const CachedArtistInfo& info)
	{
	std::lock_guard<std::mutex> lock(db_mutex_);
	SQLite::Statement ins(db_,
		"INSERT OR REPLACE INTO artist_info_cache"
		" (folder_id, mbid, last_fm_url, biography, image_url)"
		" VALUES (?, ?, ?, ?, ?)");
	ins.bind(1, folder_id);
	ins.bind(2, info.mbid);
	ins.bind(3, info.last_fm_url);
	ins.bind(4, info.biography);
	ins.bind(5, info.image_url);
	ins.exec();
	}

std::vector<MediaStore::MusicFolder> MediaStore::get_music_folders()
	{
	std::lock_guard<std::mutex> lock(db_mutex_);
	SQLite::Statement q(db_,
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
	SQLite::Statement q(db_,
		"SELECT cover_path FROM albums WHERE folder_id = ?");
	q.bind(1, folder_id);
	if (!q.executeStep() || q.getColumn(0).isNull()) return "";
	return q.getColumn(0).getString();
	}

std::optional<MediaStore::SongInfo> MediaStore::get_song(int song_id)
	{
	std::lock_guard<std::mutex> lock(db_mutex_);
	SQLite::Statement q(db_,
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
	SQLite::Statement sel(db_,
		"SELECT f.id, f.name"
		" FROM folders f"
		" WHERE f.parent_id = (SELECT id FROM folders WHERE parent_id IS NULL)"
		" ORDER BY f.name COLLATE NOCASE");

	std::vector<ArtistDir> result;
	while (sel.executeStep())
		result.push_back({sel.getColumn(0).getInt(),
		                  sel.getColumn(1).getString()});
	return result;
	}

std::optional<MediaStore::DirInfo> MediaStore::get_directory(int folder_id)
	{
	std::lock_guard<std::mutex> lock(db_mutex_);

	// Fetch the folder itself, with cover art if it's an album folder.
	SQLite::Statement fsel(db_,
		"SELECT f.id, f.name, f.parent_id,"
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
	dir.cover_art_id = fsel.getColumn(3).getInt();

	// Child directories (album folders), with artist/album names where available.
	SQLite::Statement dsel(db_,
		"SELECT f.id, f.name,"
		"       COALESCE(a.name, '') AS artist,"
		"       COALESCE(al.title, f.name) AS album,"
		"       CASE WHEN al.cover_path IS NOT NULL AND al.cover_path != ''"
		"            THEN f.id ELSE -1 END AS cover_art_id"
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
		dir.children.push_back(std::move(e));
		}

	// Child songs, with artist and album names where available.
	SQLite::Statement ssel(db_,
		"SELECT s.id, s.title, s.track_number, s.disc_number,"
		"       s.year, s.genre, s.duration, s.bitrate,"
		"       s.file_size, s.codec, s.path,"
		"       COALESCE(a.name, '') AS artist,"
		"       COALESCE(al.title, '') AS album"
		" FROM songs s"
		" LEFT JOIN albums al ON al.id = s.album_id"
		" LEFT JOIN song_artists sa ON sa.song_id = s.id AND sa.role = 'artist'"
		" LEFT JOIN artists a ON a.id = sa.artist_id"
		" WHERE s.folder_id = ?"
		" ORDER BY s.disc_number, s.track_number, s.filename");
	ssel.bind(1, folder_id);
	while (ssel.executeStep()) {
		ChildEntry e;
		e.id           = ssel.getColumn(0).getInt();
		e.parent_id    = folder_id;
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
		e.path         = ssel.getColumn(10).getString();
		e.artist       = ssel.getColumn(11).getString();
		e.album        = ssel.getColumn(12).getString();
		// Songs inherit cover art from their parent album folder.
		if (dir.cover_art_id >= 0)
			e.cover_art_id = dir.cover_art_id;
		dir.children.push_back(std::move(e));
		}

	return dir;
	}

// ---- Play queue / bookmarks ------------------------------------------

void MediaStore::save_play_queue(const std::string& username,
                                  const std::vector<int>& song_ids,
                                  int current_id, int64_t offset_ms,
                                  const std::string& client)
	{
	std::lock_guard<std::mutex> lock(db_mutex_);

	SQLite::Statement uid_q(db_, "SELECT id FROM users WHERE username = ?");
	uid_q.bind(1, username);
	if (!uid_q.executeStep()) return;
	int user_id = uid_q.getColumn(0).getInt();

	SQLite::Transaction txn(db_);

	SQLite::Statement del(db_, "DELETE FROM play_queue WHERE user_id = ?");
	del.bind(1, user_id);
	del.exec();

	SQLite::Statement ins(db_,
		"INSERT INTO play_queue (user_id, song_id, position, is_current, offset_ms, client)"
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

void MediaStore::create_bookmark(const std::string& username,
                                  int song_id, int64_t position_ms,
                                  const std::string& comment)
	{
	std::lock_guard<std::mutex> lock(db_mutex_);

	SQLite::Statement uid_q(db_, "SELECT id FROM users WHERE username = ?");
	uid_q.bind(1, username);
	if (!uid_q.executeStep()) return;
	int user_id = uid_q.getColumn(0).getInt();

	SQLite::Statement ins(db_,
		"INSERT INTO bookmarks (user_id, song_id, position, comment, changed)"
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

void MediaStore::add_star(const std::string& username,
                          int song_id, int album_id, int artist_id)
	{
	std::lock_guard<std::mutex> lock(db_mutex_);
	SQLite::Statement uid_q(db_, "SELECT id FROM users WHERE username = ?");
	uid_q.bind(1, username);
	if (!uid_q.executeStep()) return;
	int user_id = uid_q.getColumn(0).getInt();

	SQLite::Statement ins(db_,
		"INSERT OR IGNORE INTO stars (user_id, song_id, album_id, artist_id)"
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
	SQLite::Statement uid_q(db_, "SELECT id FROM users WHERE username = ?");
	uid_q.bind(1, username);
	if (!uid_q.executeStep()) return;
	int user_id = uid_q.getColumn(0).getInt();

	SQLite::Statement del(db_,
		"DELETE FROM stars WHERE user_id=?"
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
	SQLite::Statement sq(db_,
		"SELECT s.id, s.title, s.track_number, s.disc_number,"
		"       s.year, s.genre, s.duration, s.bitrate,"
		"       s.file_size, s.codec, s.folder_id,"
		"       COALESCE(a.name,'') AS artist,"
		"       COALESCE(al.title,'') AS album,"
		"       CASE WHEN al.cover_path IS NOT NULL AND al.cover_path != ''"
		"            THEN s.folder_id ELSE -1 END AS cover_art_id"
		" FROM stars st"
		" JOIN users u ON u.id = st.user_id"
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
	SQLite::Statement aq(db_,
		"SELECT f.id, COALESCE(f.parent_id,-1),"
		"       COALESCE(al.title, f.name) AS title,"
		"       COALESCE(a.name,'') AS artist,"
		"       CASE WHEN al.cover_path IS NOT NULL AND al.cover_path != ''"
		"            THEN f.id ELSE -1 END AS cover_art_id"
		" FROM stars st"
		" JOIN users u ON u.id = st.user_id"
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
	SQLite::Statement arq(db_,
		"SELECT f.id, f.name"
		" FROM stars st"
		" JOIN users u ON u.id = st.user_id"
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

	SQLite::Statement uid_q(db_, "SELECT id FROM users WHERE username = ?");
	uid_q.bind(1, username);
	if (!uid_q.executeStep()) return {};
	int user_id = uid_q.getColumn(0).getInt();

	SQLite::Transaction txn(db_);

	SQLite::Statement pins(db_,
		"INSERT INTO playlists (user_id, name) VALUES (?, ?)");
	pins.bind(1, user_id);
	pins.bind(2, name);
	pins.exec();
	int playlist_id = (int)db_.getLastInsertRowid();

	SQLite::Statement sins(db_,
		"INSERT INTO playlist_songs (playlist_id, song_id, position) VALUES (?, ?, ?)");
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

	SQLite::Statement pmeta(db_,
		"SELECT comment, created, updated FROM playlists WHERE id = ?");
	pmeta.bind(1, playlist_id);
	if (pmeta.executeStep()) {
		pl.comment = pmeta.getColumn(0).isNull() ? "" : pmeta.getColumn(0).getString();
		pl.created = pmeta.getColumn(1).getString();
		pl.updated = pmeta.getColumn(2).getString();
		}

	// Fetch songs with full metadata, same joins as get_directory.
	SQLite::Statement sq(db_,
		"SELECT s.id, s.title, s.track_number, s.disc_number,"
		"       s.year, s.genre, s.duration, s.bitrate,"
		"       s.file_size, s.codec, s.folder_id,"
		"       COALESCE(a.name,'') AS artist,"
		"       COALESCE(al.title,'') AS album,"
		"       CASE WHEN al.cover_path IS NOT NULL AND al.cover_path != ''"
		"            THEN s.folder_id ELSE -1 END AS cover_art_id"
		" FROM playlist_songs ps"
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

bool MediaStore::update_playlist(int playlist_id, const std::string& username,
                                  const std::optional<std::string>& name,
                                  const std::optional<std::string>& comment,
                                  const std::optional<bool>& is_public,
                                  const std::vector<int>& songs_to_add,
                                  const std::vector<int>& indices_to_remove)
	{
	std::lock_guard<std::mutex> lock(db_mutex_);

	// Verify ownership.
	SQLite::Statement own(db_,
		"SELECT p.id FROM playlists p"
		" JOIN users u ON u.id = p.user_id"
		" WHERE p.id = ? AND u.username = ?");
	own.bind(1, playlist_id);
	own.bind(2, username);
	if (!own.executeStep()) return false;

	// Read surviving song_ids in position order, then drop requested indices.
	SQLite::Statement sel(db_,
		"SELECT song_id FROM playlist_songs WHERE playlist_id = ? ORDER BY position");
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

	SQLite::Transaction txn(db_);

	// Apply metadata changes.
	if (name)      {
		SQLite::Statement q(db_, "UPDATE playlists SET name=? WHERE id=?");
		q.bind(1, *name); q.bind(2, playlist_id); q.exec();
		}
	if (comment)   {
		SQLite::Statement q(db_, "UPDATE playlists SET comment=? WHERE id=?");
		q.bind(1, *comment); q.bind(2, playlist_id); q.exec();
		}
	if (is_public) {
		SQLite::Statement q(db_, "UPDATE playlists SET is_public=? WHERE id=?");
		q.bind(1, *is_public ? 1 : 0); q.bind(2, playlist_id); q.exec();
		}
	{
	SQLite::Statement q(db_, "UPDATE playlists SET updated=CURRENT_TIMESTAMP WHERE id=?");
	q.bind(1, playlist_id); q.exec();
	}

	// Replace song list.
	SQLite::Statement del(db_, "DELETE FROM playlist_songs WHERE playlist_id=?");
	del.bind(1, playlist_id);
	del.exec();

	SQLite::Statement ins(db_,
		"INSERT INTO playlist_songs (playlist_id, song_id, position) VALUES (?,?,?)");
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
	SQLite::Statement q(db_,
		"SELECT p.id, p.name, COALESCE(p.comment,''), u.username, p.is_public,"
		"       COUNT(ps.song_id), COALESCE(SUM(s.duration),0),"
		"       p.created, p.updated"
		" FROM playlists p"
		" JOIN users u ON u.id = p.user_id"
		" LEFT JOIN playlist_songs ps ON ps.playlist_id = p.id"
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

	SQLite::Statement pmeta(db_,
		"SELECT p.name, COALESCE(p.comment,''), u.username, p.is_public,"
		"       p.created, p.updated"
		" FROM playlists p"
		" JOIN users u ON u.id = p.user_id"
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

	SQLite::Statement sq(db_,
		"SELECT s.id, s.title, s.track_number, s.disc_number,"
		"       s.year, s.genre, s.duration, s.bitrate,"
		"       s.file_size, s.codec, s.folder_id,"
		"       COALESCE(a.name,'') AS artist,"
		"       COALESCE(al.title,'') AS album,"
		"       CASE WHEN al.cover_path IS NOT NULL AND al.cover_path != ''"
		"            THEN s.folder_id ELSE -1 END AS cover_art_id"
		" FROM playlist_songs ps"
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
