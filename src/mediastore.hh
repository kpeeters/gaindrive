#pragma once

#include <string>
#include <set>
#include <unordered_map>
#include <vector>
#include <optional>
#include <filesystem>
#include <mutex>
#include <SQLiteCpp/SQLiteCpp.h>

#include "tmdb.hh"
#include "videoart.hh"

class MediaStore {
	public:
		// One configured library root. `name` is the identifier the user chose
		// and is also the first component of every path stored for this root —
		// that is what lets songs.path stay a single string, so the cross-DB
		// joins against client.* (which are direct string equality) never
		// needed to become two-column joins.
		//
		// Because the name is embedded in stored paths it is a durable
		// identifier: renaming a root orphans its whole subtree. It is never
		// derived from the directory basename for exactly that reason.
		struct Root {
			std::string name;   // "music", "video"; no '/', unique
			std::string type;   // "artists" | "categories" | "uploads"
			std::string path;   // absolute; normalised to have no trailing '/'
			};

		// Opens (or creates) the database and ensures the schema exists.
		// The music DB path is always derived from db_path as "<base>-music.db".
		// The user/state ("client") DB path defaults to "<base>-client.db" but
		// can be overridden by passing a non-empty user_db_path.
		// Callers must have validated `roots` (see main.cc); this constructor
		// only normalises them.
		//
		// video_art_px of 0 disables manufacturing cover art for videos
		// entirely; nothing else about a scan changes. video_art_frames and
		// video_art_embedded enable the two tiers, both off by default — see
		// videoart.hh. Turning either off also purges what it stored on an
		// earlier run, since leaving those images is indistinguishable from
		// still having the tier on.
		MediaStore(const std::string& db_path, const std::vector<Root>& roots,
		           const std::string& user_db_path = "",
		           int video_art_px = 640, bool video_art_frames = false,
		           bool video_art_embedded = false);

		// The configured roots, in the order given.
		const std::vector<Root>& roots() const;

		// The uploads root, or nullptr when none is configured. Personal
		// uploads live at <uploads root>/<username>/; with no uploads root the
		// upload endpoints refuse rather than writing into a library.
		const Root* uploads_root() const;

		// Walk every library root and upsert everything into the DB.
		// Safe to call from a background thread.
		void scan();

		// Rescan only the listed artist-level subdirectories.
		// dirs are stored-form paths "<root>/<dir>". A bare root name means the
		// root itself; if present, falls back to a full scan().
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

		// type is "artists" or "categories"; the uploads root never appears.
		struct MusicFolder { int id; std::string name; std::string type; };

		// One entry per configured library root.
		std::vector<MusicFolder> get_music_folders();

		struct CachedArtistInfo {
			std::string mbid;
			std::string last_fm_url;
			std::string biography;      // Wikipedia plain-text extract
			std::string image_url;      // Wikipedia thumbnail URL
			std::string wiki_url;       // Full Wikipedia article URL
			std::string allmusic_url;   // AllMusic artist page URL
			std::string discogs_url;    // Discogs artist page URL
			};

		// True when folder_id is a level-1 entry of a "categories" root, i.e.
		// a section like Film or Series rather than a musician. Such a folder
		// must never be looked up as an artist — the visible symptom of that
		// bug was a MusicBrainz query for a thing called "Movies".
		bool is_category_folder(int folder_id);

		// Returns empty string if folder_id not found.
		std::string get_folder_name(int folder_id);
		// Returns the stored-form folder path, or "" if not found.
		std::string get_folder_path(int folder_id);

		std::optional<CachedArtistInfo> get_cached_artist_info(int folder_id);
		void cache_artist_info(int folder_id, const CachedArtistInfo& info);

		// ---- Video cover art (see videoart.hh) ----
		//
		// Art derived from a video file by ffmpeg and cached in the music DB,
		// alongside the artist and album info caches, rather than written into
		// the library as a sidecar image.
		//
		// A song or album whose art comes from here has its cover_path set to
		// the *media file's* own path. That is a real file inside a root, so
		// path_is_within_root() and every existing "cover_path is not empty"
		// cover-art expression behave exactly as they do for a JPEG; only
		// getCoverArt has to notice the extension and read the blob instead of
		// opening the file as an image.
		struct VideoArtRow {
			std::string mime;
			std::string bytes;
			int64_t     file_modified = 0;
			};

		// path → file_modified for every cached image under a stored-form
		// prefix ("music/Artist/%"). One query per artist, mirroring the
		// known-mtimes fetch the scan already does.
		std::unordered_map<std::string, int64_t> load_video_art_keys(
			const std::string& path_prefix);
		void store_video_art(const std::string& rel_path, int64_t mtime,
		                     const std::string& mime, const std::string& source,
		                     const std::string& bytes);
		std::optional<VideoArtRow> get_video_art(const std::string& rel_path);

		// Which tier produced the stored image, or empty when there is none.
		// Separate from get_video_art() because the scan asks this of every
		// video and only wants to know whether a better tier already won —
		// get_video_art() would read the whole JPEG out of the row to answer.
		std::string get_video_art_source(const std::string& rel_path);

		// What TMDB was asked about a video and what it said. Recorded for
		// failures as much as for successes: without that, every scan would
		// re-ask about the same unmatchable file forever. `query` is what was
		// asked, so a rename produces a different question and re-runs the
		// lookup. See videoart.hh's neighbour table for the poster itself.
		struct VideoMetaRow {
			std::string query;
			std::string media_type;   // movie | tv
			int         tmdb_id = 0;
			std::string title;
			int         year    = 0;
			std::string overview;
			std::string status;       // matched | unmatched | error
			int64_t     fetched_at = 0;
			std::string poster_path;  // TMDB-relative; lets the poster be
			                          // re-fetched without a second lookup
			};
		std::optional<VideoMetaRow> get_video_meta(const std::string& rel_path);
		void store_video_meta(const std::string& rel_path,
		                      const VideoMetaRow& row);

		std::string get_setting(const std::string& key,
		                        const std::string& default_val = "");
		void set_setting(const std::string& key, const std::string& value);

		struct CachedAlbumInfo {
			std::string mbid;           // MusicBrainz release-group ID
			std::string notes;          // Wikipedia plain-text extract
			std::string wiki_url;       // Full Wikipedia article URL
			std::string allmusic_url;   // AllMusic album page URL
			};

		std::optional<CachedAlbumInfo> get_cached_album_info(int folder_id);
		void cache_album_info(int folder_id, const CachedAlbumInfo& info);

		struct ArtistDir { int id; std::string name; int album_count = 0; };

		// All level-1 folders across every library root, sorted by name. For an
		// artists root these are musicians; for a categories root they are
		// sections such as Film or Series. Root folders themselves never
		// appear.
		// personal_user non-empty → restrict to that user's subtree of the
		// uploads root; else the shared library, excluding uploads entirely.
		// music_folder_id > 0 restricts to one root (the Subsonic
		// musicFolderId filter); <= 0 spans every root, which is what an
		// unfiltered request means.
		// content_type non-empty restricts to roots of that kind — "artists"
		// or "categories". Complementary to music_folder_id rather than a
		// replacement: a kind may span several roots, which a single folder id
		// cannot express, and two artist roots must list together.
		std::vector<ArtistDir> get_artist_dirs(
			const std::string& personal_user = "",
			int music_folder_id = 0,
			const std::string& content_type = "");

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
			// Empty when not starred; otherwise the SQLite timestamp at which
			// it was starred. The API reports the time, not a flag.
			std::string starred;
			// Video only, and populated only by the paths where a client can
			// act on them: get_videos(), get_song_entry() and the browse
			// queries. Zero elsewhere, which is why the API emits
			// originalWidth/originalHeight conditionally. Whether an entry
			// *is* video is not stored here — it is derived from `codec` via
			// is_video_ext() in codecs.hh, so every existing query that
			// already selects the codec answers it for free.
			int         width  = 0;
			int         height = 0;
			// The codec pair decides whether the served stream will be
			// seekable, which the API passes to the client as nativeSeek.
			std::string video_codec;
			std::string audio_codec;
			// The season an episode belongs to; 0 for anything that is not
			// one. disc_number carries the same number — it is what clients
			// group and sort by — and this says that grouping is a season
			// rather than a disc, which is all that separates "Series 2" from
			// "Disc 2" in a listing. Populated by the same four queries as the
			// two codecs above, and zero elsewhere for the same reason.
			int         season = 0;
			};

		struct RecentSongEntry {
			ChildEntry  song;
			std::string last_played;  // ISO timestamp from play_counts
			};

		std::vector<RecentSongEntry> get_recent_songs(
			const std::string& username,
			int size   = 50,
			int offset = 0);

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
			// Artist folder_id, which is the album's own folder when that
			// folder is also an artist folder — a section holding loose files
			// is both. Emitted as `parent` and `artistId`; folder-model
			// navigation uses DirInfo::parent_id instead, which stays the
			// folder above.
			int         parent_id;
			std::string title;
			std::string artist;
			int         cover_art_id = -1;
			int         song_count   = 0;
			int         duration     = 0;  // total seconds
			int         year         = 0;
			std::string genre;
			std::string created;
			// Empty when not starred; otherwise the timestamp of the star.
			std::string starred;             // per-user (album-level star)
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
			const std::string& username = "",
			const std::string& personal_user = "");

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

		// A cover art id is a folder id, except above this base, where it is
		// SONG_COVER_ID_BASE + song id: a loose file with a sidecar image of
		// its own, whose folder cover belongs to a whole section instead. The
		// wire format is an integer, so the two id spaces are separated by an
		// offset rather than by a prefix.
		static constexpr int SONG_COVER_ID_BASE = 1'000'000'000;

		// Returns the stored-form cover image path, or "" if none.
		std::string get_cover_path(int cover_art_id);

		// Returns sorted stored-form paths of all image files in
		// the album folder tree, excluding the main cover. Used to serve
		// carousel images.
		std::vector<std::string> get_extra_image_paths(int folder_id);

		// Returns total image count for the folder (main cover + extras).
		int get_image_count(int folder_id);

		struct SongInfo {
			int         id;
			std::string path;     // stored form: "<root>/<rest>"
			std::string codec;    // e.g. "flac", "mp3"
			int         bitrate;  // kbps (from tags; 0 if unknown)
			double      duration; // seconds
			int64_t     file_size;
			// mtime as of the last scan. Part of the transcode cache key, so a
			// re-tagged or replaced file invalidates its cached transcodes
			// without anyone having to remember to purge them.
			int64_t     file_modified;
			// Video only. The streamer needs the codec pair to choose between
			// serving the file directly, remuxing it, or re-encoding it, and
			// it must be able to do that without a second DB round trip.
			bool        is_video = false;
			int         width    = 0;
			int         height   = 0;
			std::string video_codec;
			std::string audio_codec;
			};

		std::optional<SongInfo> get_song(int song_id);

		// All video rows, ordered by folder then disc/track, for getVideos.
		std::vector<ChildEntry> get_videos();

		// Subtitle streams embedded in a video file, in ffprobe stream order.
		// index is the absolute stream index for `ffmpeg -map 0:<index>`.
		struct CaptionTrack {
			int         index;
			std::string language;   // ISO 639 code, empty if untagged
			std::string title;
			};

		// Audio and subtitle tracks of one video, for getVideoInfo and
		// getCaptions. Runs ffprobe; returns empty vectors for a non-video id
		// or an unreadable file.
		struct VideoStreams {
			std::vector<CaptionTrack> captions;
			std::vector<CaptionTrack> audio_tracks;
			};

		VideoStreams get_video_streams(int song_id);

		// WebVTT for one caption source. stream_index < 0 means "the sidecar
		// subtitle file next to the video"; otherwise it is the absolute
		// ffprobe stream index of an embedded subtitle track. Conversion goes
		// through ffmpeg's webvtt muxer rather than a hand-written SRT parser.
		// Empty when there is nothing to serve.
		std::string get_captions_vtt(int song_id, int stream_index);

		// Full song metadata suitable for an API response.
		std::optional<ChildEntry> get_song_entry(int song_id);

		// ---- Music-DB id → durable path/name resolvers ----
		// Used at the REST-handler boundary to translate Subsonic-style
		// integer ids (which may change across rescans) into the durable
		// text keys used by the client-state DB (which must not). Returns
		// nullopt if the id doesn't resolve.
		std::optional<std::string> song_path_by_id(int song_id);
		std::optional<std::string> album_folder_path_by_id(int album_folder_id);
		std::optional<std::string> artist_folder_path_by_id(int artist_folder_id);

		// ---- Search ----

		struct SearchResult {
			std::vector<ChildEntry> artists;
			std::vector<ChildEntry> albums;
			std::vector<ChildEntry> songs;
			};

		SearchResult search(const std::string& query,
		                    int artist_count, int artist_offset,
		                    int album_count,  int album_offset,
		                    int song_count,   int song_offset,
		                    const std::string& personal_user = "");

		// ---- Play queue / bookmarks ----

		// Atomically replaces the user's play queue.
		// current_path is the path of the song currently playing; offset_ms is its position.
		void save_play_queue(const std::string& username,
		                     const std::vector<std::string>& song_paths,
		                     const std::string& current_path, int64_t offset_ms,
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
		void scrobble(const std::string& username, const std::string& song_path,
		              bool submission, const std::string& client);

		// Creates or updates a bookmark for a single song.
		void create_bookmark(const std::string& username,
		                     const std::string& song_path, int64_t position_ms,
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
		bool delete_bookmark(const std::string& username, const std::string& song_path);

		// Add/remove a star for the authenticated user.
		// Exactly one of song_path, album_folder_path, artist_folder_path should be non-empty.
		void add_star   (const std::string& username,
		                 const std::string& song_path,
		                 const std::string& album_folder_path,
		                 const std::string& artist_folder_path);
		void remove_star(const std::string& username,
		                 const std::string& song_path,
		                 const std::string& album_folder_path,
		                 const std::string& artist_folder_path);

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
		                             const std::vector<std::string>& song_paths);

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
		                     const std::vector<std::string>& song_paths_to_add,
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
		// path is stored-form. Returns false if no matching album exists.
		// Also marks the cover as hand-picked (albums.cover_manual), which is
		// what stops the scan replacing it with a TMDB poster.
		bool set_cover_art_path(int folder_id, const std::string& path);

		// Whether this album's cover was set through setCoverArt. Takes the
		// album folder's stored-form path, since the scan asks before Phase 4
		// has given that folder an id.
		bool cover_is_manual(const std::string& rel_album_path);

		// Compose an absolute filesystem path from a music-root-relative path.
		// All path-typed return values from MediaStore are music-root-relative;
		// callers that need to perform filesystem I/O on them go through this.
		std::string abs_path(const std::string& rel) const;

		// The inverse: an absolute filesystem path to the stored form
		// "<root name>/<rest>". Returns the input unchanged if it lies outside
		// every root. Used by FolderWatcher, which learns about changes as
		// absolute paths but must hand scan_dirs() stored-form ones.
		std::string rel_path(const std::string& abs) const;

		// Defence-in-depth check that callers MUST run on any path before
		// opening a file: returns true iff the canonicalised candidate sits
		// within the canonicalised path of ANY configured root. Resolves
		// symlinks, so a root that is itself a symlink still works, but
		// a symlink inside the tree pointing outside is detected.
		bool path_is_within_root(const std::filesystem::path& candidate) const;

	private:
		// Everything the root model needs, derived once at construction.
		// `path_slash` is the form used to compose and strip absolute paths;
		// `canonical` is what path_is_within_root() compares against, so
		// symlinks are resolved at every file open.
		struct RootRec {
			Root                  cfg;
			std::string           path_slash;   // cfg.path + '/'
			std::filesystem::path canonical;
			};
		std::vector<RootRec> roots_;
		std::vector<Root>    roots_public_;   // cfg copies, for roots()

		// Personal uploads live under the uploads root and must not surface in
		// the shared library. These are the two forms the queries need:
		// uploads_like_ is the LIKE pattern for "inside the uploads root",
		// uploads_prefix_ is the path prefix for one user's subtree. Both are
		// empty when no uploads root is configured, in which case the filters
		// are skipped entirely rather than matching everything.
		std::string uploads_like_;     // e.g. "personal/%"
		std::string uploads_prefix_;   // e.g. "personal/"

		// SQL fragment excluding the uploads root, or "" when there is none.
		// col is the qualified path column, e.g. "f.path".
		std::string not_uploads(const char* col) const;

		// Root whose filesystem path contains `abs`, or nullptr. Used on the
		// way in, when the scanner has an absolute path and needs the stored
		// form.
		const RootRec* root_for_abs(const std::string& abs) const;
		// Root named by the first component of a stored path, or nullptr.
		const RootRec* root_for_rel(const std::string& rel) const;

		// The two functions that straddle the absolute/stored boundary. Every
		// path persisted anywhere is "<root name>/<path within that root>";
		// these are the only places that know it.
		std::string strip_root(const std::string& abs) const;
		std::string join_root (const std::string& rel) const;

		SQLite::Database db_music_;
		std::mutex       db_mutex_;  // guards db_music_ across scan thread + API threads

		// Empty when cover art for videos is switched off. Held here because
		// the scan is what runs it, in a phase of its own.
		std::optional<VideoArt> video_art_;

		// The online lookup. Constructed unconditionally; it is inert until an
		// API key is configured, which is a setting rather than a start-up
		// argument, so it is re-read at the top of each scan.
		Tmdb                    tmdb_{""};

		void create_schema();

		// Drops video art produced by a tier that is now switched off, and
		// repairs the cover_path pointers left dangling by it. Called once
		// from the constructor.
		void purge_disabled_video_art();

		// Reconciles the folders table's root rows with the configuration.
		// Called once from the constructor.
		void sync_roots();

		// Targeted rescan of one artist subtree; called by scan_dirs().
		void scan_artist_dir(const std::filesystem::path& path);

		// Media files sitting directly in a root, with no section folder above
		// them. scan_artist_dir() cannot do this job: its prune prefix is
		// "<root>/%", which is the whole root.
		void scan_root_files(const RootRec& root);

		// Helpers used by scan(); all called within a single transaction.
		int  upsert_folder(const std::filesystem::path& path, int parent_id);
		int  upsert_artist(const std::string& name);
		int  upsert_album (int folder_id, const std::string& title,
		                   int artist_id, int year, const std::string& genre);
	};
