#pragma once

#include <string>
#include <set>
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

		// Rescan only the listed artist-level subdirectories.
		// If music_root_ appears in dirs, falls back to a full scan().
		void scan_dirs(const std::set<std::string>& dirs);

		// ---- User management ----

		// Returns false if username already exists.
		bool add_user(const std::string& username, const std::string& password,
		              bool is_admin = false);

		bool has_users();

		struct UserInfo {
			std::string username;
			std::string email;
			bool        is_admin;
			int         max_bitrate;
			bool        upload_allowed;
			bool        disabled;
			bool        cast_allowed;
			};

		std::optional<UserInfo> get_user(const std::string& username);

		// Returns all users ordered by username.
		std::vector<UserInfo> list_users();

		// Update mutable fields of an existing user.
		// Pass empty string for new_password to leave it unchanged.
		// Returns false if username is not found.
		bool update_user(const std::string& username,
		                 const std::string& new_password,
		                 const std::string& email,
		                 bool is_admin,
		                 int  max_bitrate,
		                 bool upload_allowed,
		                 bool disabled,
		                 bool cast_allowed);

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
			std::string biography;      // Wikipedia plain-text extract
			std::string image_url;      // Wikipedia thumbnail URL
			std::string wiki_url;       // Full Wikipedia article URL
			std::string allmusic_url;   // AllMusic artist page URL
			};

		// Returns empty string if folder_id not found.
		std::string get_folder_name(int folder_id);
		std::string get_folder_path(int folder_id);

		std::optional<CachedArtistInfo> get_cached_artist_info(int folder_id);
		void cache_artist_info(int folder_id, const CachedArtistInfo& info);

		struct CachedAlbumInfo {
			std::string mbid;           // MusicBrainz release-group ID
			std::string notes;          // Wikipedia plain-text extract
			std::string wiki_url;       // Full Wikipedia article URL
			std::string allmusic_url;   // AllMusic album page URL
			};

		std::optional<CachedAlbumInfo> get_cached_album_info(int folder_id);
		void cache_album_info(int folder_id, const CachedAlbumInfo& info);

		struct ArtistDir { int id; std::string name; int album_count = 0; };

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
			int         year         = 0;  // populated for both songs and album dirs
			// populated only when !is_dir:
			int         track_number = 0;
			int         disc_number  = 1;
			std::string genre;
			double      duration     = 0;
			int         bitrate      = 0;
			int64_t     file_size    = 0;
			std::string codec;
			std::string path;
			bool        starred      = false;
			};

		struct DirInfo {
			int         id;
			std::string name;
			int         parent_id;   // -1 if root
			int         cover_art_id = -1;
			std::vector<ChildEntry> children;
			};

		std::optional<DirInfo> get_directory(int folder_id, bool flat_multi_disc = true);

		struct AlbumEntry {
			int         id;           // folder_id (used as Subsonic album id)
			int         parent_id;    // artist folder_id
			std::string title;
			std::string artist;
			int         cover_art_id = -1;
			int         song_count   = 0;
			int         duration     = 0;  // total seconds
			int         year         = 0;
			std::string genre;
			std::string created;
			bool        starred      = false;  // per-user (album-level star)
			};

		// type: newest | random | alphabeticalByName | alphabeticalByArtist |
		//       frequent | recent | starred | byYear | byGenre
		// from_year/to_year: used for byYear; genre: used for byGenre.
		// username: required for frequent, recent, starred.
		std::vector<AlbumEntry> get_album_list(
			const std::string& type,
			int size, int offset,
			int from_year = 0, int to_year = 0,
			const std::string& genre = "",
			const std::string& username = "");

		struct ArtistInfo {
			ArtistDir               artist;
			std::vector<AlbumEntry> albums;
			};

		std::optional<ArtistInfo> get_artist(int folder_id,
		                                      const std::string& username = "");

		struct AlbumInfo {
			AlbumEntry              album;
			std::vector<ChildEntry> songs;
			};

		std::optional<AlbumInfo> get_album(int folder_id, bool flat_multi_disc = true,
		                                    const std::string& username = "");

		// Returns the filesystem path of the cover image for an album folder,
		// or empty string if none is stored.
		std::string get_cover_path(int folder_id);

		// Returns sorted paths of all image files in the album folder tree,
		// excluding the main cover. Used to serve carousel images.
		std::vector<std::string> get_extra_image_paths(int folder_id);

		// Returns total image count for the folder (main cover + extras).
		int get_image_count(int folder_id);

		struct SongInfo {
			int         id;
			std::string path;
			std::string codec;    // e.g. "flac", "mp3"
			int         bitrate;  // kbps (from tags; 0 if unknown)
			double      duration; // seconds
			int64_t     file_size;
			};

		std::optional<SongInfo> get_song(int song_id);

		// Full song metadata suitable for an API response.
		std::optional<ChildEntry> get_song_entry(int song_id);

		// ---- Search ----

		struct SearchResult {
			std::vector<ChildEntry> artists;
			std::vector<ChildEntry> albums;
			std::vector<ChildEntry> songs;
			};

		SearchResult search(const std::string& query,
		                    int artist_count, int artist_offset,
		                    int album_count,  int album_offset,
		                    int song_count,   int song_offset);

		// ---- Play queue / bookmarks ----

		// Atomically replaces the user's play queue.
		// current_id is the song currently playing; offset_ms is its position.
		void save_play_queue(const std::string& username,
		                     const std::vector<int>& song_ids,
		                     int current_id, int64_t offset_ms,
		                     const std::string& client);

		struct PlayQueue {
			int                     current_id = 0;
			int64_t                 offset_ms  = 0;
			std::string             client;
			std::string             changed;   // ISO timestamp of last save
			std::vector<ChildEntry> songs;
			};

		// Returns nullopt if the user has no saved queue.
		std::optional<PlayQueue> get_play_queue(const std::string& username);

		// Records a play. submission=true increments play_counts; false updates now_playing.
		void scrobble(const std::string& username, int song_id,
		              bool submission, const std::string& client);

		// Creates or updates a bookmark for a single song.
		void create_bookmark(const std::string& username,
		                     int song_id, int64_t position_ms,
		                     const std::string& comment);

		struct BookmarkInfo {
			ChildEntry  entry;
			int64_t     position;  // milliseconds
			std::string comment;
			std::string created;
			std::string changed;
			std::string username;
			};

		std::vector<BookmarkInfo> get_bookmarks(const std::string& username);

		// Returns false if no bookmark existed.
		bool delete_bookmark(const std::string& username, int song_id);

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

		// Returns all playlists owned by the user (songs vector is empty).
		std::vector<PlaylistInfo> get_playlists(const std::string& username);

		// Returns a single playlist with all songs populated, or nullopt if not found.
		std::optional<PlaylistInfo> get_playlist(int playlist_id,
		                                          const std::string& username = "");

		// Updates an existing playlist. Returns false if not found or not owned by username.
		// Only supplied optionals are applied. songs_to_add are appended after survivors;
		// indices_to_remove are 0-based positions removed before adding.
		bool update_playlist(int playlist_id, const std::string& username,
		                     const std::optional<std::string>& name,
		                     const std::optional<std::string>& comment,
		                     const std::optional<bool>& is_public,
		                     const std::vector<int>& songs_to_add,
		                     const std::vector<int>& indices_to_remove);

		// Returns false if not found or not owned by username.
		bool delete_playlist(int playlist_id, const std::string& username);

		// ---- Tag editing ----

		// Update title, track_number, year, and/or disc_number in the DB for a single song.
		// Returns false if song_id is not found.
		bool update_song_meta(int song_id,
		                      const std::optional<std::string>& title,
		                      const std::optional<int>& track_number,
		                      const std::optional<int>& year,
		                      const std::optional<int>& disc_number);

		// Set the cover art path for the album that owns the given folder_id.
		// Returns false if no matching album exists.
		bool set_cover_art_path(int folder_id, const std::string& path);

	private:
		std::string      music_root_;
		SQLite::Database db_music_;
		std::mutex       db_mutex_;  // guards db_music_ across scan thread + API threads

		void create_schema();

		// Targeted rescan of one artist subtree; called by scan_dirs().
		void scan_artist_dir(const std::filesystem::path& path);

		// Helpers used by scan(); all called within a single transaction.
		int  upsert_folder(const std::filesystem::path& path, int parent_id);
		int  upsert_artist(const std::string& name);
		int  upsert_album (int folder_id, const std::string& title,
		                   int artist_id, int year, const std::string& genre);
		// disc_number: 0 = default to 1, >0 = override (folder-derived disc position)
		void upsert_song  (const std::filesystem::path& path,
		                   int album_id, int folder_id, int artist_id,
		                   int disc_number = 0);
	};
