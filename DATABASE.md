
# Database files

GainDrive uses two SQLite files, both derived from the `--db` / `db_path`
base path (default `/var/lib/gaindrive/gaindrive.db`):

  * the music library DB at `<base>-music.db`
    (e.g. `gaindrive-music.db`),
  * the user/state DB at `<base>-client.db`
    (e.g. `gaindrive-client.db`), attached as the `client` schema.

The user/state DB path can be set independently with `--user-db` (CLI) or
`user_db_path` (config). When given, it overrides the `<base>-client.db`
derivation while the music DB still follows `<base>-music.db`. This is
useful for placing the small, frequently written user state on different
storage from the larger music DB that is rebuilt by library scans.


# Music database tables

**This database is a cache.** Delete it, run a full scan, and everything comes
back: every row is derived either from the filesystem or from an online lookup
that will simply be made again. Nothing a person typed or chose lives here.

That is a rule to keep rather than an accident. The two things that used to
break it were `albums.cover_manual` — a hand-picked cover, which a rebuild
threw away and a wrong TMDB match then overwrote — and a video's title, year
and episode number, which cannot be written back to the file because the
scanner reads a video's metadata from its filename and never opens it with
TagLib. Both now live in the user/state DB as `client.manual_covers` and
`client.song_meta`, and the scan re-applies them over the values it derives.

So anything new that records a *decision* belongs in the user/state DB below,
not here. The test is simple: could a full rescan of an empty database put this
value back? If not, it is in the wrong file.

```
-- The filesystem directory tree, cached.
-- Each row is one directory.
-- For music/Artist/Album/track.flac:
--   root "music" -> parent_id = NULL
--   artist dir   -> parent_id = root
--   album dir    -> parent_id = artist dir
-- There is one root row per configured library
-- root; content_type is set only on those.
CREATE TABLE folders (
    id          INTEGER PRIMARY KEY,
    parent_id   INTEGER REFERENCES folders(id),
    -- stored form: "<root name>/<rest>";
    -- a root row stores just its name
    path        TEXT NOT NULL UNIQUE,
    name        TEXT NOT NULL,
    -- root rows only: 'artists' or 'categories'.
    -- Refreshed from config at every start, so
    -- this is a cache rather than a source of truth.
    content_type TEXT,
    -- updated during scan
    last_scanned DATETIME
);

CREATE INDEX idx_folders_parent
    ON folders(parent_id);

-- Distinct people/ensembles.
-- One row per unique name. The same person
-- can appear as composer on one track and
-- performer on another — the role lives in
-- the junction tables, not here.
CREATE TABLE artists (
    id          INTEGER PRIMARY KEY,
    name        TEXT NOT NULL,
    sort_name   TEXT,  -- "Beethoven, Ludwig van"
    -- Derived by the scan from the tracks' own
    -- MUSICBRAINZ_ALBUMARTISTID, and only when every
    -- tagged track under this artist agrees; NULL
    -- otherwise, which is what a compilation gives.
    -- getArtistInfo2 skips its search-by-name when
    -- this is set. Not the same thing as
    -- artist_info_cache.mbid, which is what asking
    -- MusicBrainz produced.
    musicbrainz_id TEXT,
    image_path  TEXT,
    biography   TEXT,
    UNIQUE(name)
);

-- Albums are tied to a folder on disk.
-- The "album artist" is the folder name one
-- level up, but the actual artist links go
-- through album_artists.
CREATE TABLE albums (
    id            INTEGER PRIMARY KEY,
    folder_id     INTEGER NOT NULL
                    REFERENCES folders(id),
    title         TEXT NOT NULL,
    sort_title    TEXT,
    year          INTEGER,
    genre         TEXT,
    disc_count    INTEGER DEFAULT 1,
    duration      REAL DEFAULT 0,
    song_count    INTEGER DEFAULT 0,
    -- stored form, e.g. "music/Artist/Album/cover.jpg"
    cover_path    TEXT,
    -- NOTE: cover_manual used to sit here, marking a
    -- cover set through setCoverArt so the scan's TMDB
    -- tier would leave it alone. It is now
    -- client.manual_covers, because a rebuild of this
    -- file threw the flag away and the wrong poster
    -- won all over again.
    -- Both derived by the scan from this album's
    -- songs, and only when every tagged track agrees.
    -- They are DIFFERENT ENTITIES: a release is one
    -- pressing of one edition, a release group is the
    -- album as a work. getAlbumInfo2 looks up the
    -- second; asking /ws/2/release-group for the first
    -- is a 404.
    musicbrainz_id              TEXT,  -- release
    musicbrainz_releasegroup_id TEXT,  -- release group
    created       DATETIME DEFAULT CURRENT_TIMESTAMP,
    last_scanned  DATETIME
);

CREATE INDEX idx_albums_folder
    ON albums(folder_id);

-- Junction: which artists are associated with
-- this album, and in what capacity.
-- role is free-text but by convention:
--   'albumartist'  (primary, from folder name)
--   'composer'
--   'conductor'
--   'performer'
--   'orchestra'
CREATE TABLE album_artists (
    album_id    INTEGER NOT NULL
                  REFERENCES albums(id)
                  ON DELETE CASCADE,
    artist_id   INTEGER NOT NULL
                  REFERENCES artists(id)
                  ON DELETE CASCADE,
    role        TEXT NOT NULL DEFAULT 'albumartist',
    display_order INTEGER DEFAULT 0,
    PRIMARY KEY (album_id, artist_id, role)
);

CREATE INDEX idx_album_artists_artist
    ON album_artists(artist_id);
CREATE INDEX idx_album_artists_role
    ON album_artists(role);

CREATE TABLE songs (
    id            INTEGER PRIMARY KEY,
    album_id      INTEGER NOT NULL
                    REFERENCES albums(id)
                    ON DELETE CASCADE,
    folder_id     INTEGER NOT NULL
                    REFERENCES folders(id),
    -- stored form: "<root name>/<path within that root>"
    path          TEXT NOT NULL UNIQUE,
    filename      TEXT NOT NULL,
    -- metadata (from tags or inferred)
    title         TEXT NOT NULL,
    sort_title    TEXT,
    track_number  INTEGER,
    disc_number   INTEGER DEFAULT 1,
    year          INTEGER,
    genre         TEXT,
    -- The file's own ARTIST tag, which is a different fact from the
    -- folder-derived artist reached through song_artists: on a compilation
    -- every track has a real artist while the folder says "Various Artists".
    -- Which of the two a client is shown is decided at the API boundary.
    --
    -- NULL means "never read" and '' means "read, no tag". The distinction is
    -- what terminates the back-fill pass in the scanner's Phase 3, which is
    -- how a library scanned before this column existed acquires the tag
    -- without every file having to be touched.
    artist        TEXT,
    duration      REAL NOT NULL DEFAULT 0,
    -- audio properties
    bitrate       INTEGER,   -- kbps
    sample_rate   INTEGER,   -- Hz
    channels      INTEGER,
    codec         TEXT,      -- "flac","mp3","mkv", etc. (the extension)
    file_size     INTEGER,   -- bytes
    -- video; see VIDEO.md.  Videos share this table with audio because every
    -- piece of client state joins on songs.path, so a separate table would
    -- mean duplicating stars, play counts, playlists, queue and bookmarks.
    -- For video rows, bitrate/duration come from ffprobe rather than TagLib,
    -- and video_codec/audio_codec are what the streamer's tier ladder reads
    -- to decide between serving directly, remuxing, and re-encoding.
    is_video      INTEGER DEFAULT 0,
    width         INTEGER DEFAULT 0,   -- 0 when unprobed
    height        INTEGER DEFAULT 0,
    video_codec   TEXT,      -- "h264","mpeg2video", etc. (ffprobe codec_name)
    -- Set for videos, and for the audio containers that can hold more than one
    -- codec -- .m4a (AAC or ALAC) and .ogg/.oga (Vorbis, Opus, FLAC, Speex),
    -- where the extension does not say which.  That is the half a `playable`
    -- container/codec declaration is matched against; see codecs.hh.  Filled
    -- from the extension alone for .mp3/.flac/.opus/.aac, which settle it.
    -- NULL means "never read" and '' means "read, could not tell", the same
    -- distinction `artist` above uses, and for the same reason: it is what
    -- terminates the scanner's Phase 3 back-fill on an existing library.
    audio_codec   TEXT,      -- "aac","alac","vorbis","ac3", etc.
    -- The season an episode belongs to, from an S02E03 marker or a folder
    -- naming its season; 0 for anything that is not one. disc_number holds
    -- the same number, because that is what clients group and sort by; this
    -- says the grouping is a season rather than a disc, which is what lets a
    -- client head it "Series 2". A "Disc 2" or "CD1" folder is deliberately
    -- not a season, and neither is an unnumbered "Specials".
    season        INTEGER DEFAULT 0,
    -- Sidecar image beside this file, stored form; empty for a song that
    -- inherits its album's cover. Set for a loose file — one sitting directly
    -- in a section or a root, whose folder cover belongs to the whole section
    -- rather than to it. Reached as cover art id
    -- MediaStore::SONG_COVER_ID_BASE + songs.id, since a cover art id is
    -- otherwise a folders.id and the wire format is a plain integer.
    cover_path    TEXT,
    -- cover art embedded in file
    has_embedded_cover INTEGER DEFAULT 0,
    -- MusicBrainz ids out of this file's own tags,
    -- read in Phase 3 through TagLib's PropertyMap.
    -- NULL means not known -- there is no back-fill
    -- pass, so a row scanned before these existed
    -- keeps NULL until the file changes or this DB is
    -- rebuilt. A tag naming more than one entity, or
    -- not shaped like a UUID, is discarded.
    musicbrainz_id              TEXT,  -- recording
    musicbrainz_album_id        TEXT,  -- release
    musicbrainz_releasegroup_id TEXT,
    musicbrainz_albumartist_id  TEXT,
    -- timestamps
    file_modified DATETIME,
    created       DATETIME DEFAULT CURRENT_TIMESTAMP,
    last_scanned  DATETIME
);

CREATE INDEX idx_songs_album
    ON songs(album_id);
CREATE INDEX idx_songs_folder
    ON songs(folder_id);
CREATE INDEX idx_songs_genre
    ON songs(genre);

-- Junction: which artists appear on this
-- specific track. This is where "feat." and
-- classical roles live.
--
-- A typical classical track might have:
--   (song, Hilary Hahn,     'performer')
--   (song, Gustavo Dudamel, 'conductor')
--   (song, LASO,            'orchestra')
--   (song, Beethoven,       'composer')
--
-- A pop track with a feature:
--   (song, Drake,       'artist')
--   (song, Rihanna,     'artist')
CREATE TABLE song_artists (
    song_id     INTEGER NOT NULL
                  REFERENCES songs(id)
                  ON DELETE CASCADE,
    artist_id   INTEGER NOT NULL
                  REFERENCES artists(id)
                  ON DELETE CASCADE,
    role        TEXT NOT NULL DEFAULT 'artist',
    display_order INTEGER DEFAULT 0,
    PRIMARY KEY (song_id, artist_id, role)
);

CREATE INDEX idx_song_artists_artist
    ON song_artists(artist_id);
CREATE INDEX idx_song_artists_role
    ON song_artists(role);

-- Cover art for a video: a poster fetched from TMDB
-- (source 'tmdb'), or one of the two local tiers of
-- VideoArt (src/videoart.hh) — the image embedded in
-- the container, with --video-art-embedded, or a
-- representative frame, with --video-art-frames.
-- Both local tiers are off by default; turning one
-- off deletes the rows it wrote. Video files carry
-- no tag anything writes and are rarely named well,
-- so without this every video shows a placeholder.
--
-- It lives here with artist_info_cache and
-- album_info_cache rather than being written into
-- the library as a sidecar image: nothing gaindrive
-- derives should land in the user's collection.
-- Deleting the music database throws it away and
-- the next scan rebuilds it, exactly as it does for
-- fetched artist biographies.
--
-- Keyed on the stored path, not on songs.id, for
-- the reason stars and playlists are: a rowid is
-- not stable across a rescan. file_modified is
-- what invalidates the image when the file is
-- replaced or re-encoded.
--
-- A song or album whose art comes from here has
-- its cover_path set to the *media file's* own
-- path. That is a real file inside a root, so
-- every existing "cover_path is not empty"
-- cover-art expression, and path_is_within_root(),
-- behave as they do for a JPEG; only getCoverArt
-- has to notice the extension and read the blob.
CREATE TABLE video_art (
    path          TEXT PRIMARY KEY,   -- "<root>/<rest>"
    file_modified INTEGER NOT NULL,
    mime          TEXT NOT NULL,
    -- embedded | frame | tmdb; kept so a bad batch
    -- of one kind can be deleted and regenerated,
    -- which is what happens to every 'frame' row on
    -- startup when the frame tier is off
    source        TEXT NOT NULL,
    image         BLOB NOT NULL,
    created_at    INTEGER NOT NULL
                    DEFAULT (strftime('%s','now'))
);

-- Scaled cover art. Every client asks getCoverArt
-- for a pixel size, and before this table each of
-- those requests forked ffmpeg and decoded the
-- full-size source — on every request, for ever.
--
-- Keyed on the stored path, like video_art and for
-- the same reason: a rowid moves across a rescan.
-- One key space serves all three kinds of art,
-- because a directory and a file cannot share a
-- path:
--   * a cover or extra image -> its own path
--   * a video_art blob       -> the media file's
--     path, which is already what cover_path holds
--   * an artist portrait     -> the artist folder
--
-- source_stamp is deliberately NOT in the key.
-- (source_key, size) being the key makes a
-- re-encode after a cover is replaced an
-- INSERT OR REPLACE rather than a second row, so a
-- file edited a hundred times leaves one row per
-- size and not a hundred.
--
-- `size` is quantised to a ladder before it gets
-- here. That is the table's only bound: size is an
-- unvalidated client integer and there is no
-- eviction policy, so without the ladder any
-- account could write a row per pixel value.
--
-- It is the SHORT edge, so the long one overshoots
-- it. Every surface that asks for a size crops the
-- result to a square, and fitting the long edge
-- leaves the client upscaling a 2:3 poster to fill
-- its cell. Nothing in the key says which rule
-- made a row, so changing it means dropping them
-- all -- see MUSIC_CACHE_VERSION in mediastore.cc.
--
-- status 'unscalable' records an image neither stb
-- nor ffmpeg could decode, with a zero-length blob,
-- so it is not retried on every request. Same
-- reasoning as video_meta's 'unmatched'.
CREATE TABLE cover_thumbs (
    source_key   TEXT    NOT NULL,   -- "<root>/<rest>"
    size         INTEGER NOT NULL,   -- short edge, quantised
    source_stamp INTEGER NOT NULL,
    status       TEXT    NOT NULL,   -- ok | unscalable
    mime         TEXT    NOT NULL,
    width        INTEGER NOT NULL,
    height       INTEGER NOT NULL,
    image        BLOB    NOT NULL,
    created_at   INTEGER NOT NULL
                   DEFAULT (strftime('%s','now')),
    PRIMARY KEY (source_key, size)
);

-- The artist portrait itself, rather than a URL to
-- it. artist_info_cache stores an image_url, which
-- is a promise a third party may not keep; the
-- bytes lived only in a map in the HTTP server, so
-- every restart re-fetched every portrait, and a
-- getCoverArt for an unresolved artist ran the
-- whole provider chain inside the request thread.
--
-- Keyed on the artist folder's path. Filled by a
-- background thread, so serving one is a plain
-- read. The image is normalised to at most 800px
-- on the long edge when stored.
--
-- status records a failure as much as a success:
-- 'none' means the providers had nothing and is
-- not re-asked for 30 days, 'error' means the
-- network failed and is retried at once.
CREATE TABLE artist_art (
    folder_path TEXT PRIMARY KEY,   -- "<root>/<artist>"
    name        TEXT NOT NULL,      -- what was asked
    status      TEXT NOT NULL,      -- ok | none | error
    -- wikipedia | wikidata | theaudiodb | discogs
    source      TEXT NOT NULL DEFAULT '',
    source_url  TEXT NOT NULL DEFAULT '',
    mime        TEXT NOT NULL DEFAULT '',
    width       INTEGER NOT NULL DEFAULT 0,
    height      INTEGER NOT NULL DEFAULT 0,
    image       BLOB,
    fetched_at  INTEGER NOT NULL
                  DEFAULT (strftime('%s','now'))
);

-- What TMDB was asked about a video, and what it
-- said. The filename parser (src/videoname.hh)
-- produces the question; a match supplies the
-- poster (stored in video_art above, with
-- source='tmdb'), the plot (written into
-- album_info_cache.notes, where getAlbumInfo2
-- already looks) and the canonical title.
--
-- `path` is the *album folder* for a film in a
-- folder of its own, and the song for a loose file
-- in a section: a film is a folder, so it is one
-- question however many parts it was split into.
--
-- The row exists as much to record a failure as a
-- success. Without it every scan would re-ask about
-- the same unmatchable file forever; with it, a
-- rescan of an identified library costs no traffic
-- at all. `query` is what was asked, so renaming a
-- file poses a different question and the lookup
-- runs again.
--
-- status: matched | unmatched | error. Only 'error'
-- is retried (after a day) — 'unmatched' is a
-- judgement about the name, and asking again
-- tomorrow would get the same answer.
CREATE TABLE video_meta (
    path        TEXT PRIMARY KEY,   -- "<root>/<rest>"
    query       TEXT NOT NULL,      -- "title|year|type"
    media_type  TEXT NOT NULL,      -- movie | tv
    tmdb_id     INTEGER,
    title       TEXT,
    year        INTEGER,
    overview    TEXT,
    -- lets the poster be re-fetched without asking
    -- who this is a second time
    poster_path TEXT,
    status      TEXT NOT NULL,
    -- TMDB's genres, joined with '|'. Our own
    -- encoding rather than a tag, so the separator
    -- is safe: no TMDB genre contains one.
    --
    -- NULL means "asked before this column existed"
    -- and '' means "asked, TMDB had none". The
    -- distinction is what makes the one-time
    -- back-fill terminate -- with the two conflated
    -- either every matched film is re-asked on every
    -- scan for ever, or no film in an existing
    -- library ever gets a genre. Same rule, and same
    -- reason, as songs.artist.
    genre       TEXT,
    fetched_at  INTEGER NOT NULL
                  DEFAULT (strftime('%s','now'))
);

-- The song markers inside one video, as an
-- *index*.
--
-- The sidecar <stem>.chapters.txt beside the video
-- is the authority and always has been:
-- getChapters reads that file on every call,
-- because it is a per-playback lookup that has to
-- be right. This table is what lets *browsing*
-- avoid it -- the album view lists a concert's
-- songs from one query, and search can match a
-- chapter title, neither of which could afford a
-- file read (or, for a rip with no sidecar, an
-- ffprobe) per video per request.
--
-- It passes the test at the top of this file:
-- every row is re-derived from a file on disk, so
-- deleting this database and rescanning puts all
-- of it back. Nothing a person typed lives only
-- here -- what they typed is in the sidecar, which
-- sits in the library tree and travels with it.
--
-- Keyed on the stored path rather than songs.id,
-- like video_art and for the reason stars are: a
-- rowid does not survive a rescan. And with no
-- foreign key, also like video_art --
-- album_info_cache's FK to folders(id) with no
-- cascade is what once made DELETE FROM folders
-- fail and roll an entire scan back.
--
-- Read in scan Phase 1, beside the sidecar cover,
-- and deliberately not in Phase 3: that phase sees
-- only files whose mtime changed, and a sidecar is
-- written without touching the media file's, so an
-- edit would never be noticed -- nor would a library
-- scanned before this existed ever be back-filled.
--
-- Audio as well as video. A DJ set or a mixtape is
-- one file holding a dozen songs for exactly the
-- reason a concert film is. load_chapter_keys() is
-- what keeps that affordable: without it every audio
-- row would issue a DELETE here on every scan to
-- discover it has no markers.
--
-- Only sidecars are indexed. A container's own
-- chapters still reach the player through
-- getChapters, but indexing them would mean
-- -show_chapters on every probe, which
-- read_video_probe()'s "unusable" retry test does
-- not cover, so a list a narrow -probesize missed
-- would be recorded silently as none.
CREATE TABLE chapters (
    path  TEXT    NOT NULL,   -- "<root>/<rest>"
    idx   INTEGER NOT NULL,   -- 1-based, by start
    start REAL    NOT NULL,   -- seconds
    title TEXT    NOT NULL,   -- may be empty
    PRIMARY KEY (path, idx)
);

-- Every genre a song carries, in source order.
--
-- songs.genre survives beside this and holds the
-- *first* of them: it is the single-valued Subsonic
-- `genre` field, and what albums.genre rolls up
-- from, so keeping it is what leaves the eleven
-- ChildEntry queries untouched. This table is the
-- full list, and it is what getGenres,
-- getSongsByGenre and getAlbumList type=byGenre
-- read.
--
-- Multi-value is not a nicety for video: a film is
-- normally two or three genres -- Alien is Horror
-- *and* Science Fiction -- and filing it under only
-- the first is the loss this exists to prevent.
-- Audio reaches it too, from a multi-valued Vorbis
-- GENRE or ID3v2 TCON. A "Rock/Pop" *string* is
-- still one genre: there the separator is a guess
-- about somebody else's intent, and gaindrive does
-- not guess.
--
-- Two counts follow from it and must agree, which
-- is why they are defined as each other:
-- getGenres' albumCount is albums *having* a song
-- of the genre, exactly what type=byGenre's EXISTS
-- returns. Counting albums whose own albums.genre
-- matched would leave a mostly-Horror film out of
-- Science Fiction.
--
-- Keyed on the stored path, no foreign key, for the
-- reasons chapters and video_art give above.
--
-- Names are trimmed on write, so readers fold case
-- alone and a plain NOCASE index serves.
--
-- Seeded once from songs.genre the first time the
-- table appears. Without that an upgraded install
-- shows no genres at all, because Phase 3 only
-- opens files whose mtime changed and would never
-- re-read the rest; the multi-value each file may
-- carry then arrives per file as files change, or
-- on a rebuild.
CREATE TABLE song_genres (
    path TEXT    NOT NULL,   -- "<root>/<rest>"
    idx  INTEGER NOT NULL,   -- 1-based; 1 is primary
    name TEXT    NOT NULL,   -- trimmed on write
    PRIMARY KEY (path, idx)
);
```

# User-facing and API state tables

```
CREATE TABLE users (
    id            INTEGER PRIMARY KEY,
    username      TEXT NOT NULL UNIQUE,
    -- md5(password) stored; the Subsonic
    -- token scheme recomputes md5(pass+salt)
    -- so you need the raw password or its
    -- reversible form. Store encrypted.
    password_enc  TEXT NOT NULL,
    email         TEXT,
    is_admin      INTEGER DEFAULT 0,
    max_bitrate   INTEGER DEFAULT 0,
    created       DATETIME DEFAULT CURRENT_TIMESTAMP,
    last_access   DATETIME
);

-- All references to music-library rows are by
-- stored-form path "<root name>/<rest>",
-- never by integer rowid. SQLite forbids foreign
-- keys across attached databases anyway, and
-- using stored-form paths means the user-state DB
-- survives:
--   * a music-DB rebuild (fresh rowids)
--   * the rowid churn from the song scanner's
--     INSERT OR REPLACE on path conflict
--   * moving a root to a different on-disk
--     location (its NAME must not change:
--     that is what these paths are keyed on)
--
-- The music DB stores paths in the same stored
-- form, so cross-DB JOINs are direct equality:
--   JOIN songs s ON s.path = st.song_path
CREATE TABLE stars (
    user_id            INTEGER NOT NULL
                         REFERENCES users(id)
                         ON DELETE CASCADE,
    -- exactly one of these three is non-NULL
    song_path          TEXT,
    album_folder_path  TEXT,
    artist_folder_path TEXT,
    created            DATETIME
                         DEFAULT CURRENT_TIMESTAMP,
    UNIQUE(user_id, song_path,
           album_folder_path, artist_folder_path)
);

CREATE TABLE play_counts (
    user_id     INTEGER NOT NULL
                  REFERENCES users(id)
                  ON DELETE CASCADE,
    song_path   TEXT NOT NULL,
    count       INTEGER DEFAULT 0,
    last_played DATETIME,
    PRIMARY KEY (user_id, song_path)
);

CREATE TABLE playlists (
    id          INTEGER PRIMARY KEY,
    user_id     INTEGER NOT NULL
                  REFERENCES users(id)
                  ON DELETE CASCADE,
    name        TEXT NOT NULL,
    comment     TEXT,
    is_public   INTEGER DEFAULT 0,
    created     DATETIME DEFAULT CURRENT_TIMESTAMP,
    updated     DATETIME DEFAULT CURRENT_TIMESTAMP
);

CREATE TABLE playlist_songs (
    playlist_id INTEGER NOT NULL
                  REFERENCES playlists(id)
                  ON DELETE CASCADE,
    song_path   TEXT NOT NULL,
    position    INTEGER NOT NULL,
    PRIMARY KEY (playlist_id, position)
);

CREATE TABLE play_queue (
    user_id     INTEGER NOT NULL
                  REFERENCES users(id)
                  ON DELETE CASCADE,
    song_path   TEXT NOT NULL,
    position    INTEGER NOT NULL,
    is_current  INTEGER DEFAULT 0,
    offset_ms   INTEGER DEFAULT 0,
    client      TEXT,
    updated     DATETIME DEFAULT CURRENT_TIMESTAMP,
    PRIMARY KEY (user_id, position)
);

-- For scrobble / "now playing"
CREATE TABLE now_playing (
    user_id     INTEGER NOT NULL
                  REFERENCES users(id)
                  ON DELETE CASCADE,
    song_path   TEXT NOT NULL,
    client      TEXT,
    started     DATETIME DEFAULT CURRENT_TIMESTAMP,
    PRIMARY KEY (user_id)
);

CREATE TABLE bookmarks (
    user_id     INTEGER NOT NULL
                  REFERENCES users(id)
                  ON DELETE CASCADE,
    song_path   TEXT NOT NULL,
    position    INTEGER NOT NULL DEFAULT 0,
    comment     TEXT,
    created     DATETIME DEFAULT CURRENT_TIMESTAMP,
    changed     DATETIME DEFAULT CURRENT_TIMESTAMP,
    PRIMARY KEY (user_id, song_path)
);

-- A cover a person chose through setCoverArt.
--
-- The *image* survives a rebuild of the music DB --
-- it is written into the album folder as cover.jpg
-- -- but "a human picked this" is recorded nowhere
-- on disk, and it is the only thing keeping the
-- scan's TMDB poster tier off a hand-picked cover.
-- Since an upload is also the way to fix a wrong
-- TMDB match, losing the flag removes the remedy.
--
-- No user_id, unlike stars: the image is written
-- into the library tree and every user sees it, so
-- the choice belongs to the library. That makes this
-- a global row like settings.
--
-- Keyed on the album folder's stored-form path
-- rather than a rowid, because cover_is_manual() is
-- asked in scan Phase 3c -- before Phase 4 has
-- upserted the folder and so before any id exists.
--
-- Only the flag is stored, not the chosen path:
-- setCoverArt normalises the on-disk name to
-- cover.jpg or cover.png and deletes the loser, so
-- find_cover() re-derives the same image anyway.
CREATE TABLE manual_covers (
    album_folder_path TEXT PRIMARY KEY,
    created           DATETIME
                        DEFAULT CURRENT_TIMESTAMP
);

-- Metadata a person typed for a *video*: the one
-- kind of file whose edit cannot be written back to
-- the thing it describes.
--
-- read_song_metadata() returns after the ffprobe
-- branch and never reaches TagLib, so the scanner
-- takes a video's title, year and episode number
-- from its filename and never from its tags. A tag
-- written here would be a second copy of a fact
-- that nothing reads -- so updateSong deliberately
-- does not write one, even for an .mp4, which
-- TagLib could open.
--
-- Applied over the scanned values inside the album
-- transaction by apply_song_meta_overrides(), so the
-- music DB still holds the effective title and every
-- read query stays as it was. That is what makes
-- this a cache-only fix rather than a join in a
-- dozen queries that must never drift.
--
-- NULL means "not overridden", per column: editing a
-- title must not blank a year edited earlier.
--
-- Keyed on the song's stored-form path, like stars,
-- because a rowid does not survive the rebuild this
-- table exists to make safe.
CREATE TABLE song_meta (
    song_path    TEXT PRIMARY KEY,
    title        TEXT,
    track_number INTEGER,
    year         INTEGER,
    disc_number  INTEGER,
    changed      DATETIME
                   DEFAULT CURRENT_TIMESTAMP
);
```
