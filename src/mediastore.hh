#pragma once

#include <string>
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

	private:
		std::string      music_root_;
		SQLite::Database db_;
		std::mutex       db_mutex_;  // guards db_ across scan thread + future API threads

		void create_schema();

		// Helpers used by scan(); all called within a single transaction.
		int  upsert_folder(const std::filesystem::path& path, int parent_id);
		int  upsert_artist(const std::string& name);
		int  upsert_album (int folder_id, const std::string& title,
		                   int artist_id, int year, const std::string& genre);
		void upsert_song  (const std::filesystem::path& path,
		                   int album_id, int folder_id);
	};
