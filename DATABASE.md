
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
    musicbrainz_id TEXT,
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
    audio_codec   TEXT,      -- "aac","ac3", etc.
    -- cover art embedded in file
    has_embedded_cover INTEGER DEFAULT 0,
    -- timestamps
    musicbrainz_id TEXT,
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
```
