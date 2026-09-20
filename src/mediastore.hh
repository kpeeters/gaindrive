#pragma once

#include <atomic>
#include <chrono>
#include <string>
#include <set>
#include <unordered_map>
#include <vector>
#include <optional>
#include <filesystem>
#include <mutex>
#include <system_error>
#include <SQLiteCpp/SQLiteCpp.h>

#include "chapters.hh"
#include "tmdb.hh"
#include "videoart.hh"

// A dot-prefixed name is never library content.  macOS writes "._Track01.mp3"
// beside every file it copies to a foreign filesystem: an AppleDouble resource
// fork carrying the same extension, which is all the scanner's admission test
// looks at.  It also keeps out .DS_Store, a synced tree's .stversions and a
// leftover .users.
bool is_hidden_name(const std::filesystem::path& p);

// The marker a producer holds while a personal upload batch is still being
// rearranged: "<uploads>/<user>/<uuid>.inflight", beside the batch directory
// and deliberately not inside it, because reparent_loose_media() sweeps every
// regular file at the top of a batch down into <artist>/.  The archive staging
// file "<uuid>.upload.part" sits beside a batch for the same reason.
//
// On disk rather than in memory because the thing it guards against outlives a
// process: a server killed mid-fetch must not come back and index the part
// files the download tool left behind, several of which end in .m4a and would
// otherwise each earn a songs row.  It says one thing only -- this batch's
// directory layout is still moving -- and it is released after the fold and
// before the scan, which is what stops scan_batch() skipping its own batch.
std::filesystem::path batch_marker(const std::filesystem::path& batch);

bool batch_held(const std::filesystem::path& batch);

// One album as Phase 1 read it off the disk. Defined in mediastore.cc, because
// it is the scanner's own working type and nothing outside the scan needs it;
// declared here only so commit_album() can take one.
struct AlbumReadData;

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
		// scan_jobs bounds how many files the scan reads metadata from at
		// once; 0 derives it from the core count, capped — see scan_jobs_
		// below for why the cap is about the disk. Appended rather than
		// inserted: the other construction sites pass positionally.
		MediaStore(const std::string& db_path, const std::vector<Root>& roots,
		           const std::string& user_db_path = "",
		           int video_art_px = 640, bool video_art_frames = false,
		           bool video_art_embedded = false, int scan_jobs = 0);

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

		// What getScanStatus reports. `count` is songs processed, and it keeps
		// the finished total once a scan ends — which is what the spec's
		// "scanning: false, count: N" is for.
		struct ScanStatus { bool scanning; long long count; };
		ScanStatus scan_status() const;

		// ---- User management ----

		// Letters, digits, '.', '_' and '-', at most 64 bytes. A username is a
		// directory component in the uploads tree and the key every ownership
		// check compares against, so it cannot be an arbitrary string.
		static bool valid_username(const std::string& u);

		// The user/state database this db_path implies.  Public because
		// --install-service has to check that file exists before writing a unit
		// that runs as somebody who may not be able to create it, and a second
		// copy of the rule would be a check aimed at a file nothing ever opens --
		// note --db itself names no file that is opened.
		static std::string client_db_path(const std::string& db_path);

		// Returns false if the username already exists or is not valid.
		// p=enc:HEX decoded, p passed through, nullopt for malformed hex.
		// Shared by validate_auth and the endpoints that *store* a password.
		static std::optional<std::string> decode_enc_password(const std::string& p);

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

		// One root, by the id get_music_folders() reported for it.
		//
		// Answers nullopt for anything that is not a browsable library root —
		// a folder deeper in the tree, an id that resolves to nothing, and the
		// uploads root, which that listing excludes for the same reason. Both
		// go through the same rule so a caller cannot be handed a root the
		// listing would never have offered: moveAlbum takes this id straight
		// from a client, and the uploads root is per-user space rather than a
		// destination anyone browses to — moving something *into* it would put
		// a shared-library album inside somebody's personal area.
		std::optional<MusicFolder> music_folder_by_id(int id);

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

		// The MusicBrainz ids the *scan* derived from the files' own tags, as
		// against the ones in the info caches above, which came from asking
		// MusicBrainz. Empty when the folder's tracks carried none or did not
		// agree; both callers then fall back to the online search.
		//
		// Keyed on the folder id, which is what every caller has in hand — the
		// artist and album rows are reached from it, not the other way about.
		//
		// The album one returns the *release group*, not the release, because
		// that is the entity getAlbumInfo2 looks up. They are different things
		// and asking /ws/2/release-group for a release id is a 404.
		std::string get_artist_tag_mbid(int folder_id);
		std::string get_album_tag_releasegroup_mbid(int folder_id);

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

		// The paths under `path_prefix` that already have chapter rows, one
		// query per artist, exactly as load_video_art_keys() above works.
		//
		// It exists to keep the scan's inner loop free: without it, every
		// audio row in the library would execute a DELETE against `chapters`
		// on every scan just in case it had some. With it, a song is touched
		// only when it has markers now or had them before.
		std::set<std::string> load_chapter_keys(const std::string& path_prefix);
		// poster_path is which TMDB image the bytes are; empty for the local
		// tiers, where the file itself is the identity.
		void store_video_art(const std::string& rel_path, int64_t mtime,
		                     const std::string& mime, const std::string& source,
		                     const std::string& bytes,
		                     const std::string& poster_path = "");
		std::optional<VideoArtRow> get_video_art(const std::string& rel_path);

		// Which tier produced the stored image and, for a poster, which TMDB
		// image it is; source is empty when there is no row. Separate from
		// get_video_art() because the scan asks this of every video and only
		// wants to know whether the right image already won; get_video_art()
		// would read the whole JPEG out of the row to answer.
		struct VideoArtState {
			std::string source;
			std::string poster_path;
			};
		VideoArtState get_video_art_state(const std::string& rel_path);

		// ---- Scaled cover art (see imagescale.hh) ----
		//
		// getCoverArt is asked for a pixel size by every client there is, and
		// before this table each of those requests forked ffmpeg and decoded
		// the full-size source from scratch. Keyed on the stored path, not on
		// folders.id, for the reason video_art and stars are: a rowid moves
		// across a rescan.
		//
		// `source_stamp` is the source's mtime, or an artist portrait row's
		// fetched_at. It is deliberately *not* part of the key: (source_key,
		// size) being the key is what makes re-encoding after a cover is
		// replaced an INSERT OR REPLACE rather than a second row, so a file
		// edited a hundred times leaves one row per size and not a hundred.
		struct ThumbRow {
			std::string status;    // ok | unscalable
			std::string mime;
			std::string bytes;     // empty when status is "unscalable"
			int         width  = 0;
			int         height = 0;
			int64_t     source_stamp = 0;
			};

		// A row whose source_stamp disagrees with the source is still
		// returned; only the caller knows what the source's stamp is now, so
		// only the caller can call that a miss.
		std::optional<ThumbRow> get_cover_thumb(const std::string& source_key,
		                                        int size);
		void store_cover_thumb(const std::string& source_key, int size,
		                       int64_t source_stamp, const std::string& status,
		                       const std::string& mime, int width, int height,
		                       const std::string& bytes);

		// Every size for one source. For the changes an mtime cannot show:
		// a video_art blob regenerated by a better tier, or a cover uploaded
		// through setCoverArt twice within one filesystem timestamp tick.
		void drop_cover_thumbs(const std::string& source_key);

		// ---- Artist portraits ----
		//
		// The bytes, not a URL. Keyed on the artist folder's stored path, so
		// getCoverArt can answer without opening a socket and a restart does
		// not re-download the whole library's portraits.
		struct ArtistArtRow {
			std::string status;       // ok | none | error
			std::string source;       // wikipedia | wikidata | theaudiodb | discogs
			std::string source_url;
			std::string mime;
			std::string bytes;        // empty unless status is "ok"
			int         width  = 0;
			int         height = 0;
			int64_t     fetched_at = 0;
			};
		std::optional<ArtistArtRow> get_artist_art(const std::string& folder_path);

		// Status without the image. Separate for the reason
		// get_video_art_state() is: the serving path asks "is there one yet"
		// far more often than it needs the bytes, and reading a JPEG out of a
		// row to answer that would undo the point of the table.
		struct ArtistArtState {
			std::string status;
			int64_t     fetched_at = 0;
			};
		std::optional<ArtistArtState> get_artist_art_state(
			const std::string& folder_path);

		void store_artist_art(const std::string& folder_path,
		                      const std::string& name, const ArtistArtRow& row);

		// One folder for the online resolver to work through: an artist to be
		// looked up, or an album. Only ever produced by the three queries
		// below, all of which restrict themselves to *artists* roots — a
		// categories section is called "Film", and asking MusicBrainz about
		// that is the mistake is_category_folder() exists to prevent, while a
		// film's own description comes from TMDB during the scan instead.
		//
		// That is a test on the root, not on the album, so a concert or a music
		// video filed under the performer is admitted. It should be:
		// MusicBrainz catalogues official concert releases as release groups,
		// and the query is anchored on the artist, which is the half that
		// discriminates.
		struct LookupTarget {
			int         folder_id = 0;
			std::string name;
			std::string path;
			};
		// Artist folders with no usable portrait yet, in name order — what the
		// background resolver works through on its own timer.
		std::vector<LookupTarget> artists_needing_art(int64_t retry_none_before);

		// The two backfill queries behind startInfoLookup, asking the question
		// artists_needing_art() does not: not "has this ever been resolved"
		// but "are there any *words*". An artist whose portrait arrived while
		// Wikipedia was down has a cached row, an 'ok' art verdict, an empty
		// biography and nothing that will ever ask again — which is how a
		// library ends up with hundreds of them.
		//
		// Neither consults fetched_at. An admin pressing the button means it,
		// and re-asking is also what makes a spell of provider failure
		// recoverable; the cost is that a second press re-asks about everyone
		// nobody has written about.
		std::vector<LookupTarget> artists_needing_bio();
		std::vector<LookupTarget> albums_needing_info();

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
			// TMDB's genres, joined with '|'. Empty is a real answer -- some
			// titles have none -- which is why it needs the flag below rather
			// than being its own signal.
			std::string genre;
			// False when the stored column is NULL, meaning the row was
			// written before genres were asked for at all. That is a
			// different thing from "asked, none", and the difference is what
			// makes the one-time back-fill terminate: without it either every
			// matched film is re-asked on every scan, or no film in an
			// existing library ever gets a genre. Set only by
			// get_video_meta(); a freshly built row leaves it false and is
			// about to be filled in anyway.
			bool        genre_known = false;
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

		struct ArtistDir {
			int id;
			std::string name;
			int album_count = 0;
			// Which account's uploads this came from. Empty for the shared
			// library, and empty for a single-user personal listing too — the
			// caller already knows whose that is. Set only under
			// PERSONAL_ALL_USERS, where it is the only thing telling two
			// identically named folders apart.
			std::string owner;
			};

		// The personal_user value meaning "every account's uploads", for an
		// admin looking at what there is to approve. Not a username: '*' cannot
		// be one, because sanitise_component() would never produce it.
		static constexpr const char* PERSONAL_ALL_USERS = "*";

		// All level-1 folders across every library root, sorted by name. For an
		// artists root these are musicians; for a categories root they are
		// sections such as Film or Series. Root folders themselves never
		// appear.
		// personal_user non-empty → restrict to that user's subtree of the
		// uploads root, or to every user's under PERSONAL_ALL_USERS; else the
		// shared library, excluding uploads entirely.
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
			std::string artist;   // the folder-derived artist
			// The file's own ARTIST tag, raw and undecided.  Whether it
			// differs from `artist` above is a presentation question, answered
			// in one place — the song entry writers in gaindrive.cc, which are
			// the only code holding both facts at once.
			//
			// Every ChildEntry query selects it, as they now all select the
			// codec pair and the season beside it: a track shown away from its
			// album is exactly where this is most needed.
			std::string track_artist;
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
			// queries. Zero elsewhere — unlike the codec pair below, which
			// every song query now selects — which is why the API emits
			// originalWidth/originalHeight conditionally. Whether an entry
			// *is* video is not stored here — it is derived from `codec` via
			// is_video_ext() in codecs.hh, so every existing query that
			// already selects the codec answers it for free.
			int         width  = 0;
			int         height = 0;
			// The codec pair decides whether the served stream will be
			// seekable, which the API passes to the client as nativeSeek.
			//
			// **Every song query selects these**, via SONG_VIDEO_COLS_SQL, and
			// that is newer than it looks.  Four queries used to, and a video
			// reached through getStarred, getPlaylist, getPlayQueue,
			// getBookmarks, getSongsByGenre, getRecentSongs or search came back
			// with both empty — so video_seeks_natively() answered false for a
			// file that seeks perfectly well.  The same file therefore answered
			// differently depending on which endpoint was asked, which is not a
			// difference any client can be expected to reason about: the
			// Android app hides its cast button on it, and transcode_target()
			// advertises a conversion that will not happen.
			std::string video_codec;
			std::string audio_codec;
			// The season an episode belongs to; 0 for anything that is not
			// one. disc_number carries the same number — it is what clients
			// group and sort by — and this says that grouping is a season
			// rather than a disc, which is all that separates "Series 2" from
			// "Disc 2" in a listing. Selected by the same fragment as the two
			// codecs above, and for the same reason.
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

		struct NowPlayingEntry {
			ChildEntry  song;
			std::string username;
			std::string client;      // player name, from scrobble's c= param
			std::string song_path;   // relative path, for the uploads read check
			int         minutes_ago = 0;
			};

		std::vector<NowPlayingEntry> get_now_playing();

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
			// Artist folder_id: the folder above the album, always. A loose
			// file is its own album and its parent is the section, so the
			// case this used to carry — a folder that was an album *and* an
			// artist folder — no longer exists. Emitted as `parent` and
			// `artistId`; folder-model navigation uses DirInfo::parent_id
			// instead, which is the same folder here.
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
			// How many of this album's songs are video. Nothing else on an
			// album says so -- isVideo is a song field, derived from the
			// codec extension -- so without it a client cannot tell a season
			// of a series from a record without fetching its tracks.
			int         video_count  = 0;
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
		// SONG_COVER_ID_BASE + song id: a song carrying a sidecar image of its
		// own, whose album's cover is somebody else's. The wire format is an
		// integer, so the two id spaces are separated by an offset rather than
		// by a prefix.
		//
		// Still needed now that a loose file is its own album and reaches its
		// sidecar through that album's cover_path: read_song_file() calls
		// find_song_cover() for *every* song, so "01 song.jpg" beside
		// "01 song.flac" inside an ordinary album folder still lands here.
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
			// Audio only: the container the scan observed, which is a
			// different fact from `codec` above -- that is the filename's
			// extension, and .ogg and .oga are one container under two of
			// them. The pair (this, audio_codec) is what a `playable`
			// declaration is compared against; see AudioForm in codecs.hh.
			std::string audio_container;
			};

		std::optional<SongInfo> get_song(int song_id);

		// All video rows, ordered by folder then disc/track, for getVideos.
		std::vector<ChildEntry> get_videos();

		// One row of getGenres. `name` is the commonest raw spelling of the
		// tag; the counts are over the folded form, so "Rock" and "rock" are
		// one genre with one set of counts.
		struct GenreEntry {
			std::string name;
			int         song_count  = 0;
			int         album_count = 0;
			};

		// Every distinct genre in the shared library, commonest first.
		//
		// Deliberately reports the tags as they are, including "unknown",
		// "other" and outright typos: this is the only view that shows a
		// person their own tagging, and a list quietly filtered to the
		// plausible entries would hide exactly what needs correcting. Folding
		// case and padding is not the same thing -- those spellings are the
		// same tag by anyone's account, and no information is lost by
		// counting them together.
		std::vector<GenreEntry> get_genres();

		// The songs carrying one genre, matched on the folded spelling so the
		// value taken from get_genres() always matches.
		std::vector<ChildEntry> get_songs_by_genre(const std::string& genre,
		                                            int count, int offset);

		// One selectable caption source. index is the absolute ffprobe stream
		// index for `ffmpeg -map 0:<index>`, or SIDECAR_CAPTION_INDEX for the
		// subtitle file sitting beside the video.
		struct CaptionTrack {
			int         index;
			std::string language;   // ISO 639 code, empty if untagged
			std::string title;
			};

		// A sidecar is not a stream, so it needs an id no stream can have, and
		// it has to survive the round trip through the wire as a plain integer
		// — which is why it is negative rather than, say, a name. getCaptions
		// reads any index < 0 as "the sidecar", so an old client that omits
		// captionId altogether still lands on the same file.
		static constexpr int SIDECAR_CAPTION_INDEX = -1;

		// Audio and subtitle tracks of one video, for getVideoInfo and
		// getCaptions. Runs ffprobe; returns empty vectors for a non-video id
		// or an unreadable file.
		struct VideoStreams {
			std::vector<CaptionTrack> captions;
			std::vector<CaptionTrack> audio_tracks;
			};

		VideoStreams get_video_streams(int song_id);

		// Chapter markers for one video: the sidecar beside it, or failing that
		// whatever its container carries.
		//
		// `source` says which, because they are not the same fact. A sidecar was
		// put there deliberately and so wins outright -- the rule
		// sidecar_captions() already follows -- while a container's own chapters
		// are read-only: editing a rip writes a sidecar that shadows them, and
		// deleting that file restores them. An *empty* sidecar is therefore a
		// tombstone rather than a missing one, which is the only way to say "this
		// film has no chapters" about a rip whose container disagrees.
		//
		// Not cached, for the reason get_video_streams() gives: a per-playback
		// lookup, not a browse path. In particular do not cache on song_id -- a
		// rebuild reassigns every rowid.
		struct VideoChapters {
			std::vector<Chapter> chapters;
			std::string          source;   // "sidecar", "container" or "none"
			};

		VideoChapters get_chapters(int song_id);

		// Writes the sidecar for one video, replacing whatever was there. The
		// caller is responsible for sanitising the titles -- utf8_clean() lives
		// at the API boundary, where the other arbitrary strings are cleaned.
		//
		// An empty list writes an empty file rather than removing it, so that
		// clearing the markers of a rip does not make its container chapters
		// reappear. False means nothing was written and the old file stands.
		bool save_chapters(int song_id, const std::vector<Chapter>& chapters);

		// Every chaptered item in one album folder, from the `chapters`
		// index, in the order the album lists its tracks.
		//
		// Not "video": a two-hour DJ set fetched as audio carries markers for
		// exactly the same reason a concert film does.
		//
		// The index, not the files: this is a browse path, and reading a
		// sidecar per video -- or worse, running ffprobe for one without --
		// every time somebody opens an album is the cost the table exists to
		// remove. get_chapters() above stays the authority for playback.
		struct AlbumChapterItem {
			int                  song_id;
			std::string          title;
			double               duration;   // the video's, for the last span
			std::vector<Chapter> chapters;
			};

		std::vector<AlbumChapterItem> get_album_chapters(int folder_id);

		// The most markers one file may hold, in both directions. A hand-written
		// file goes straight into a JSON document, so the ceiling is as much a
		// read-path bound as a write-path one.
		static constexpr size_t MAX_CHAPTERS = 1000;

		// The sidecar's name is <stem> + this, and it is a constant rather than a
		// literal because getAlbumTexts has to skip these files: it lists every
		// .txt in an album folder as liner notes, and it cannot use
		// sidecar_chapters_path() because it works from a folder rather than from
		// a song. Two spellings of the name would mean chapter files silently
		// reappearing as prose the day it changed.
		static constexpr std::string_view CHAPTERS_SUFFIX = ".chapters.txt";

		// The chapter file beside the video at `abs`. The single place that
		// knows what "beside" means for chapters, as sidecar_captions() is for
		// subtitles: get_chapters(), save_chapters() and the scan all go
		// through it, so a list that disagreed with what a save wrote cannot
		// arise.
		//
		// Unlike sidecar_captions() this is not replace_extension(): a plain
		// <stem>.txt is indistinguishable from liner notes, which is a file
		// people really do put in album folders.
		//
		// Public and static because scanning reads these files in Phase 1,
		// from a free function with no MediaStore in hand. It takes an
		// absolute path and returns one, so it is the rare public member that
		// speaks in them -- the stored-path convention is about what leaves
		// the API, and nothing here does.
		static std::string sidecar_chapters_path(const std::string& abs);

		// The liner-note sidecar of a file-album: <stem>.txt, beside the media
		// file. Absolute in and out, as sidecar_chapters_path() is.
		static std::string sidecar_text_path(const std::string& abs);

		// Every sidecar belonging to one media file -- cover, poster,
		// subtitles, chapters, liner notes -- absolute, and only the ones that
		// exist. Used by moveAlbum and deleteUpload, which move or remove a
		// single-file album and must take its companions with it.
		std::vector<std::string> sidecars_of(const std::string& abs) const;

		// Whether this folder row has an albums row of its own. The question
		// getCoverArt has to ask before treating a coverless folder as an
		// artist and asking MusicBrainz about its name.
		bool folder_is_album(int folder_id);

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

		// The inverse, for a handler that has just moved a folder and needs to
		// tell the client where it went.
		std::optional<int> folder_id_by_path(const std::string& rel);

		// ---- Search ----

		// A chapter that matched a search, with enough context to draw a row
		// and enough to act on one.
		//
		// Deliberately not a ChildEntry: a chapter is not a song, and putting
		// one in the song list would hand every client a track it cannot
		// stream, star or queue. It is reported in an array of its own.
		struct ChapterHit {
			int         song_id;      // the video it is inside
			int         parent_id;    // that video's album folder
			int         index;        // 1-based, as getChapters numbers them
			double      start;
			std::string name;
			std::string track;        // the title of the song or film it is in
			std::string album;
			std::string artist;
			};

		struct SearchResult {
			std::vector<ChildEntry> artists;
			std::vector<ChildEntry> albums;
			std::vector<ChildEntry> songs;
			std::vector<ChapterHit> chapters;
			};

		SearchResult search(const std::string& query,
		                    int artist_count, int artist_offset,
		                    int album_count,  int album_offset,
		                    int song_count,   int song_offset,
		                    int chapter_count, int chapter_offset,
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

		// Set the cover art path for the album at the given folder. Both
		// arguments are stored-form. Returns false if no matching album
		// exists -- but the choice is recorded either way.
		//
		// Records the choice in client.manual_covers, which is where it
		// belongs: the music DB is a cache and may be deleted, and this is the
		// one thing a rescan could not put back. It is what stops the scan
		// replacing a hand-picked cover with a TMDB poster.
		bool set_cover_art_path(const std::string& rel_folder,
		                        const std::string& path);

		// Whether this album's cover was set through setCoverArt. Takes the
		// album folder's stored-form path, since the scan asks before Phase 4
		// has given that folder an id.
		bool cover_is_manual(const std::string& rel_album_path);

		// Record metadata a person typed for a video, whose edit cannot be
		// written back to the file: the scanner takes a video's title, year
		// and episode number from its filename and never reads its tags. The
		// override is applied over the scanned values during each scan, so
		// deleting the music DB and rescanning still yields the typed value.
		// Omitted fields are left as they were, so editing one does not blank
		// another.
		void set_song_meta_override(const std::string& rel_song_path,
		                            const std::optional<std::string>& title,
		                            const std::optional<int>& track_number,
		                            const std::optional<int>& year,
		                            const std::optional<int>& disc_number);

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

		// The subtitle file beside the video at `abs`, or empty if there is
		// none. The single place that knows what "beside" means: both
		// get_video_streams(), which offers it, and get_captions_vtt(), which
		// serves it, go through here, and a listing that disagreed with the
		// server would be a caption track that 404s when selected.
		std::string sidecar_captions(const std::string& abs) const;

		// Rewrite every stored path at or under old_rel to sit under new_rel,
		// in both databases. This is what makes renaming a directory on disk
		// something other than data loss: every reference to a media file in
		// either DB is the path string itself, so the rename invalidates the
		// key. The caller MUST have verified that nothing already lives at
		// new_rel — see the note in the definition on why that precondition is
		// what keeps these plain UPDATEs safe.
		//
		// Anything new that persists a path belongs in this function's list.
		bool relocate_prefix(const std::string& old_rel,
		                     const std::string& new_rel);

		// Forget every *client* row at or under rel, for a directory that has
		// been deleted rather than moved. Stars, play counts, playlist entries,
		// the queue, now-playing, bookmarks, hand-picked covers and typed
		// video metadata.
		//
		// The music DB is deliberately absent: a following scan_dirs() prunes
		// the folder, its songs and every derived cache keyed on the path
		// (video_art, video_meta, cover_thumbs, artist_art). **Nothing anywhere
		// prunes the client schema** — the scanner never touches it — which is
		// the whole reason this exists.
		//
		// Leaving those rows behind is not merely untidy. They are read through
		// INNER JOINs, so an orphan is invisible rather than broken; but a
		// personal batch that is folded into an earlier one can put a *new* file
		// at exactly the path a deleted one had, and a surviving star would then
		// attach itself to it. Anything new that persists a path in the client
		// schema belongs in this function's list as well as relocate_prefix's.
		void forget_prefix(const std::string& rel);

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

		// What scan_status() reports. Held here rather than in GainDrive
		// because this is where all four scan entry points converge — the
		// startup scan, the folder watcher, the upload handler and the URL
		// fetch worker — and any of them can be running at once.
		//
		// A depth *count*, not a flag: scan_dirs() falls back to calling
		// scan() when it cannot resolve a path, so a bool would be cleared by
		// the inner call returning while the outer one was still going.
		std::atomic<int>       scans_active_{0};
		std::atomic<long long> scan_items_{0};

		// Where a scan's time went: one microsecond accumulator per phase of
		// scan_artist_dir(), reported on the "Scan complete" line.  Purely
		// diagnostic, and it exists because a first scan of a cold library is
		// minutes of work with nothing anywhere saying which phase they were
		// spent in — so every judgement about scan cost was a guess, including
		// the obvious ones.
		//
		// Atomic for the reason scan_items_ is, and for one more: the phases
		// timed here are exactly the ones a parallel scan would thread, and an
		// accumulator that had to be revisited then would be the wrong shape.
		//
		// They inherit scan_items_' caveat.  scans_active_ is a depth count and
		// not an exclusion lock — the folder watcher can start a rescan while
		// the start-up scan is still running — so two overlapping scans add
		// into one set of figures.  That is acceptable for a diagnostic, but it
		// is worth knowing before reading a surprising number.
		struct ScanTimes {
			std::atomic<long long> walk  {0};  // Phase 1: the disk walk
			std::atomic<long long> known {0};  // Phase 2: the known-mtime read
			std::atomic<long long> meta  {0};  // Phase 3: TagLib and ffprobe
			std::atomic<long long> art   {0};  // Phase 3b: video cover art
			std::atomic<long long> tmdb  {0};  // Phase 3c: the online lookups
			std::atomic<long long> write {0};  // Phase 4: the album transactions
			std::atomic<long long> prune {0};  // Phase 4: the mark and the prune
			std::atomic<long long> files {0};  // files whose metadata was read
			std::atomic<long long> videos{0};  // of those, the ffprobe forks
			std::atomic<long long> albums{0};  // albums committed

			// Phase 3 split by what actually read the file: TagLib in this
			// process against an ffprobe fork.  Summed per file across the
			// workers, so these are **thread**-microseconds and do not add up
			// to `meta`, which is the phase's wall clock — with N jobs
			// saturated they approach N times it.  That is the point: the
			// ratio between them says which half of the library the phase is
			// actually spent on, which one wall-clock figure covering 25k
			// audio files and 2k videos cannot.
			std::atomic<long long> meta_audio{0};
			std::atomic<long long> meta_video{0};
			void reset();
			};
		ScanTimes scan_times_;

		// The breakdown as one wrapped log line.  wall_s is passed in rather
		// than measured here because a nested scan_dirs() has its own.
		std::string scan_times_report(double wall_s) const;

		// Increments scans_active_ for its lifetime, and zeroes the item count
		// and the phase timings on the transition into a scan so both mean
		// "this scan" rather than "since start-up".
		struct ScanGuard {
			MediaStore& s;
			explicit ScanGuard(MediaStore& st);
			~ScanGuard();
			};

		// When each user's last_access was last written. Guarded by db_mutex_,
		// which validate_auth() holds anyway. See the comment there for why
		// this is a map and not a WHERE clause.
		std::unordered_map<std::string, std::chrono::steady_clock::time_point>
			last_access_seen_;

		// How wide Phase 3 of a scan runs — the TagLib and ffprobe reads,
		// which measured 80% of a cold scan.
		//
		// Derived from the core count when the caller passes 0, but **capped
		// at 8, and the cap is about the disk rather than the CPU**: a
		// sixteen-core machine has the same one head as a two-core one, and 16
		// jobs measured no better than 8 on the machine the knee was found on.
		// The floor of 2 also absorbs hardware_concurrency() returning 0.
		// CoverArtCache caps its decode pool the same way and for an
		// analogous reason. 1 takes the sequential path through parallel_for,
		// which is what makes it a usable answer to "is this bug the
		// parallelism?".
		int scan_jobs_ = 4;

		// Empty when cover art for videos is switched off. Held here because
		// the scan is what runs it, in a phase of its own.
		std::optional<VideoArt> video_art_;

		// Converted WebVTT, kept because producing it is not cheap: ffmpeg has
		// to demux the whole container to collect one subtitle stream, which on
		// a feature-length mkv is seconds, and getCaptions is asked for the
		// same track repeatedly — by a <track> element that reloads on every
		// transcoded seek, and by a Chromecast that fetches it the moment the
		// viewer turns captions on and would give up long before a cold
		// conversion finished.
		//
		// In memory rather than in the DB or the transcode cache: a subtitle
		// file is tens of kilobytes, it is derived data that costs one ffmpeg
		// run to rebuild, and a process that has just started has nothing to
		// serve stale. Keyed on file_modified as well as the id, so re-tagging
		// or replacing the file invalidates it — the same key TranscodeCache
		// entries carry, for the same reason.
		//
		// Bounded because nothing else bounds it: a library has as many
		// subtitle tracks as it has films.
		struct CaptionKey
			{
			int     song_id;
			int64_t file_modified;
			int     stream_index;
			bool operator==(const CaptionKey& o) const
				{
				return song_id == o.song_id
				    && file_modified == o.file_modified
				    && stream_index == o.stream_index;
				}
			};
		struct CaptionKeyHash
			{
			std::size_t operator()(const CaptionKey& k) const
				{
				return std::hash<int64_t>()(k.file_modified)
				     ^ (std::hash<int>()(k.song_id) << 1)
				     ^ (std::hash<int>()(k.stream_index) << 2);
				}
			};
		static constexpr std::size_t CAPTION_CACHE_MAX = 32;
		std::mutex captions_mutex_;   // its own: nothing here touches the DB
		std::unordered_map<CaptionKey, std::string, CaptionKeyHash> captions_cache_;
		std::vector<CaptionKey> captions_order_;   // insertion order, for eviction

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
		// them -- each one an album of its own, as it would be in a section.
		// scan_artist_dir() cannot do this job: its prune prefix is
		// "<root>/%", which is the whole root.
		//
		// It is also the *only* owner of those albums, and scan() and
		// scan_dirs() both keep them away from scan_artist_dir() for that
		// reason: their folder rows name files, so that function's
		// fs::is_directory() test would read every one of them as deleted.
		void scan_root_files(const RootRec& root);

		// One album's whole commit: its folder row, its album row, its songs,
		// the per-album song sweep and everything derived from those. Takes
		// db_mutex_ and runs its own transaction, so the caller supplies only
		// the try/catch that keeps one failed album from costing the scan.
		//
		// Shared by scan_artist_dir() and scan_root_files() because a loose
		// file directly in a root is an album in exactly the way a loose file
		// in a section is, and the copy this replaced in scan_root_files()
		// quietly skipped apply_album_video_name() and the year and genre
		// roll-ups — so a film in a flat library was the one film that never
		// got its parsed title.
		void commit_album(const AlbumReadData& adat, int parent_folder_id,
		                   int artist_id,
		                   const std::set<std::string>& chapter_keys);

		// Helpers used by scan(); all called within a single transaction.
		int  upsert_folder(const std::filesystem::path& path, int parent_id,
		                   const std::string& name_override = "");
		int  upsert_artist(const std::string& name);
		int  upsert_album (int folder_id, const std::string& title,
		                   int artist_id, int year, const std::string& genre);
	};
