#pragma once

#include <string>
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
