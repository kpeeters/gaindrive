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
			std::vector<ChildEntry> children;
			};

		std::optional<DirInfo> get_directory(int folder_id);

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
		                   int album_id, int folder_id);
	};
