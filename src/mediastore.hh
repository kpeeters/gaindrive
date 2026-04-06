#pragma once

#include <string>
#include <vector>
#include <optional>
#include <filesystem>
#include <mutex>
#include <SQLiteCpp/SQLiteCpp.h>

class MediaStore {
	public:
		// Opens (or creates) the database and ensures the schema exists.
		MediaStore(const std::string& db_path, const std::string& music_root);

		// Walk music_root_ and upsert everything into the DB.
		// Safe to call from a background thread.
		void scan();

		// ---- User management ----

		// Returns false if username already exists.
		bool add_user(const std::string& username, const std::string& password,
		              bool is_admin = false);

		bool has_users();

		struct UserInfo {
			std::string username;
			std::string email;
			bool        is_admin;
			};

		std::optional<UserInfo> get_user(const std::string& username);

		// Validates Subsonic auth params. Supply either password (from p=,
		// possibly with "enc:" prefix) or token+salt (from t= and s=).
		bool validate_auth(const std::string& username,
		                   const std::string& password,
		                   const std::string& token,
		                   const std::string& salt);

		// ---- Library browsing ----

		struct MusicFolder { int id; std::string name; };

		// The configured music root(s) — currently always one entry.
		std::vector<MusicFolder> get_music_folders();

		struct CachedArtistInfo {
			std::string mbid;
			std::string last_fm_url;
			std::string biography;   // Wikipedia plain-text extract
			std::string image_url;   // Wikipedia thumbnail URL
			};

		// Returns empty string if folder_id not found.
		std::string get_folder_name(int folder_id);

		std::optional<CachedArtistInfo> get_cached_artist_info(int folder_id);
		void cache_artist_info(int folder_id, const CachedArtistInfo& info);

		struct ArtistDir { int id; std::string name; };

		// All artist-level folders (depth-1 children of root), sorted by name.
		std::vector<ArtistDir> get_artist_dirs();

		struct ChildEntry {
			int         id;
			int         parent_id;
			bool        is_dir;
			std::string title;    // folder name or song title
			std::string artist;
			std::string album;
			int         cover_art_id = -1;  // folder_id for getCoverArt; -1 = none
			// populated only when !is_dir:
			int         track_number = 0;
			int         disc_number  = 1;
			int         year         = 0;
			std::string genre;
			double      duration     = 0;
			int         bitrate      = 0;
			int64_t     file_size    = 0;
			std::string codec;
			std::string path;
			};

		struct DirInfo {
			int         id;
			std::string name;
			int         parent_id;   // -1 if root
			int         cover_art_id = -1;
			std::vector<ChildEntry> children;
			};

		std::optional<DirInfo> get_directory(int folder_id);

		// Returns the filesystem path of the cover image for an album folder,
		// or empty string if none is stored.
		std::string get_cover_path(int folder_id);

		struct SongInfo {
			int         id;
			std::string path;
			std::string codec;    // e.g. "flac", "mp3"
			int         bitrate;  // kbps (from tags; 0 if unknown)
			double      duration; // seconds
			int64_t     file_size;
			};

		std::optional<SongInfo> get_song(int song_id);

		// ---- Play queue / bookmarks ----

		// Atomically replaces the user's play queue.
		// current_id is the song currently playing; offset_ms is its position.
		void save_play_queue(const std::string& username,
		                     const std::vector<int>& song_ids,
		                     int current_id, int64_t offset_ms,
		                     const std::string& client);

		// Creates or updates a bookmark for a single song.
		void create_bookmark(const std::string& username,
		                     int song_id, int64_t position_ms,
		                     const std::string& comment);

		// Add/remove a star for the authenticated user.
		// Exactly one of song_id, album_id, artist_id should be non-zero.
		void add_star   (const std::string& username, int song_id, int album_id, int artist_id);
		void remove_star(const std::string& username, int song_id, int album_id, int artist_id);

		struct StarredResult {
			std::vector<ArtistDir>  artists;
			std::vector<ChildEntry> albums;
			std::vector<ChildEntry> songs;
			};

		StarredResult get_starred(const std::string& username);

		// ---- Playlists ----

		struct PlaylistInfo {
			int         id;
			std::string name;
			std::string comment;
			std::string owner;
			bool        is_public;
			int         song_count;
			int         duration;   // total seconds
			std::string created;
			std::string updated;
			std::vector<ChildEntry> songs;
			};

		// Creates a new playlist for the user and returns it fully populated.
		PlaylistInfo create_playlist(const std::string& username,
		                             const std::string& name,
		                             const std::vector<int>& song_ids);

	private:
		std::string      music_root_;
		SQLite::Database db_;
		std::mutex       db_mutex_;  // guards db_ across scan thread + API threads

		void create_schema();

		struct Counts { int artists = 0; int albums = 0; int files = 0; };

		// Quick directory-only walk; opens no files.
		Counts count_audio_files();

		// Helpers used by scan(); all called within a single transaction.
		int  upsert_folder(const std::filesystem::path& path, int parent_id);
		int  upsert_artist(const std::string& name);
		int  upsert_album (int folder_id, const std::string& title,
		                   int artist_id, int year, const std::string& genre);
		void upsert_song  (const std::filesystem::path& path,
		                   int album_id, int folder_id, int artist_id);
	};
