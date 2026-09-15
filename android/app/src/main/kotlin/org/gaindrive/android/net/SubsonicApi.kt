package org.gaindrive.android.net

import retrofit2.http.GET
import retrofit2.http.Query

/**
 * The Subsonic endpoints the app calls. Auth and protocol parameters are added
 * by [AuthInterceptor], so they never appear here.
 *
 * Endpoints marked as gaindrive extensions have no Subsonic equivalent; a
 * server that does not implement them answers with an error, which callers
 * degrade to an empty section rather than surfacing.
 */
interface SubsonicApi {

	// ── System ──────────────────────────────────────────────────────────

	@GET("rest/ping.view")
	suspend fun ping(): SubsonicEnvelope<PingBody>

	@GET("rest/getUser.view")
	suspend fun getUser(@Query("username") username: String): SubsonicEnvelope<GetUserBody>

	// ── Browsing ────────────────────────────────────────────────────────

	// No default arguments anywhere in this interface. Kotlin implements them
	// with a synthetic bridge, and Retrofit only proxies the abstract method —
	// it works, but it is a subtlety not worth relying on. Callers pass null
	// or an empty list explicitly.

	@GET("rest/getMusicFolders.view")
	suspend fun getMusicFolders(): SubsonicEnvelope<GetMusicFoldersBody>

	@GET("rest/getArtists.view")
	suspend fun getArtists(
		/** gaindrive extension: restricts the list to the user's own uploads. */
		@Query("personal") personal: String?,
		/**
		 * gaindrive extension: restricts the list to roots of one kind,
		 * "artists" or "categories". Null returns every root's children mixed,
		 * which is what a client with no concept of categories gets. In
		 * practice mutually exclusive with [personal] — uploads are their own
		 * root and are never part of the shared library.
		 */
		@Query("contentType") contentType: String?,
		/**
		 * Restricts to one root. Finer than [contentType], which names a *kind*
		 * and may span several roots — and that is exactly why it is here: a
		 * promote destination is one specific root, so the folder suggestions
		 * for it cannot be asked for by kind.
		 */
		@Query("musicFolderId") musicFolderId: String?,
	): SubsonicEnvelope<GetArtistsBody>

	@GET("rest/getArtist.view")
	suspend fun getArtist(@Query("id") id: String): SubsonicEnvelope<GetArtistBody>

	@GET("rest/getAlbum.view")
	suspend fun getAlbum(@Query("id") id: String): SubsonicEnvelope<GetAlbumBody>

	/**
	 * One track by id. Browsing never needs it — a listing already carries its
	 * rows — so its caller is [org.gaindrive.android.data.TrackLinkResolver],
	 * where a link arrives holding nothing but an id.
	 */
	@GET("rest/getSong.view")
	suspend fun getSong(@Query("id") id: String): SubsonicEnvelope<GetSongBody>

	@GET("rest/getArtistInfo2.view")
	suspend fun getArtistInfo2(@Query("id") id: String): SubsonicEnvelope<GetArtistInfoBody>

	// ── Browsing by folder ──────────────────────────────────────────────
	//
	// The same hierarchy read from the directory tree rather than from tags,
	// for a server whose ID3 tags are patchy. Selected per server; see
	// `data/browse/BrowseSource.kt` for which endpoints pair with which.

	/**
	 * The folder-browsing counterpart of [getArtists]: the level-1 entries of
	 * every root, grouped into index buckets. Carries no `albumCount` — the
	 * server does not count albums it was not asked to enumerate.
	 */
	@GET("rest/getIndexes.view")
	suspend fun getIndexes(
		/** Restricts to one music folder. Null spans every one of them. */
		@Query("musicFolderId") musicFolderId: String?,
		/** gaindrive extension, honoured here exactly as on [getArtists]. */
		@Query("contentType") contentType: String?,
		/** gaindrive extension, honoured here exactly as on [getArtists]. */
		@Query("personal") personal: String?,
	): SubsonicEnvelope<GetIndexesBody>

	/**
	 * One directory's children: subdirectories and tracks in a single mixed
	 * array, told apart by `isDir`. Serves as both "the albums of an artist"
	 * and "the tracks of an album" — which level it is depends only on the id.
	 */
	@GET("rest/getMusicDirectory.view")
	suspend fun getMusicDirectory(@Query("id") id: String): SubsonicEnvelope<GetMusicDirectoryBody>

	@GET("rest/getAlbumInfo2.view")
	suspend fun getAlbumInfo2(@Query("id") id: String): SubsonicEnvelope<GetAlbumInfoBody>

	// ── Video ───────────────────────────────────────────────────────────

	/**
	 * The subtitle and audio streams inside one video.
	 *
	 * The server runs `ffprobe` on every call rather than answering from the
	 * scan, so this is slower than it looks — call it once per video load, not
	 * per listing. `getCaptions` is not here: it returns raw WebVTT rather than
	 * a Subsonic envelope, and its URL is handed to the player rather than
	 * fetched by the app.
	 */
	@GET("rest/getVideoInfo.view")
	suspend fun getVideoInfo(@Query("id") id: String): SubsonicEnvelope<GetVideoInfoBody>

	// ── Chapters ────────────────────────────────────────────────────────
	//
	// A gaindrive extension. The song boundaries inside one long recording — a
	// concert, a DJ set, a mixtape — which are what let a listing name the
	// songs rather than the file. Neither endpoint is video-only: an audio
	// track carries markers for exactly the same reason a film does.

	/**
	 * The markers inside one recording, read from the file itself.
	 *
	 * The authority, and the one that costs a file read — use it on the
	 * playback path, where the list has to be right, and not in a listing.
	 * It is also the only one that can see a video's *container* chapters,
	 * which the scan does not index.
	 */
	@GET("rest/getChapters.view")
	suspend fun getChapters(@Query("id") id: String): SubsonicEnvelope<GetChaptersBody>

	/**
	 * Every chaptered item in one album folder, from the scan's index.
	 *
	 * The browse counterpart of [getChapters]: cheap enough to ask on every
	 * album open, and returned in the order the album listing already draws, so
	 * the two zip together without re-sorting. Only sidecars are indexed, so a
	 * video whose markers live solely in its container is absent here and
	 * present there.
	 */
	@GET("rest/getAlbumChapters.view")
	suspend fun getAlbumChapters(@Query("id") id: String): SubsonicEnvelope<GetAlbumChaptersBody>

	// ── Searching ───────────────────────────────────────────────────────

	/**
	 * [chapterCount] is a gaindrive extension and the server defaults it to 0,
	 * unlike its three siblings — so asking for chapter matches is opt-in and a
	 * client that does not want them pays for no extra query. `chapterOffset`
	 * exists too and is deliberately not declared: nothing here pages a search,
	 * and an unused parameter would suggest otherwise.
	 */
	@GET("rest/search3.view")
	suspend fun search3(
		@Query("query") query: String,
		@Query("artistCount") artistCount: Int,
		@Query("albumCount") albumCount: Int,
		@Query("songCount") songCount: Int,
		@Query("chapterCount") chapterCount: Int,
	): SubsonicEnvelope<Search3Body>

	/**
	 * The folder-browsing counterpart of [search3], and the reason it exists
	 * here: `search3` answers with ID3 ids, which on a server that keeps the two
	 * hierarchies apart are not the ids `getMusicDirectory` accepts. Tapping a
	 * result would then open the wrong album rather than fail — both id spaces
	 * are integers, so nothing detects the mismatch. Folder mode asks the
	 * endpoint that answers in its own ids.
	 */
	@GET("rest/search2.view")
	suspend fun search2(
		@Query("query") query: String,
		@Query("artistCount") artistCount: Int,
		@Query("albumCount") albumCount: Int,
		@Query("songCount") songCount: Int,
		@Query("chapterCount") chapterCount: Int,
	): SubsonicEnvelope<Search2Body>

	// ── Playlists ───────────────────────────────────────────────────────

	@GET("rest/getPlaylists.view")
	suspend fun getPlaylists(): SubsonicEnvelope<GetPlaylistsBody>

	@GET("rest/getPlaylist.view")
	suspend fun getPlaylist(@Query("id") id: String): SubsonicEnvelope<GetPlaylistBody>

	@GET("rest/createPlaylist.view")
	suspend fun createPlaylist(
		@Query("name") name: String,
		@Query("songId") songIds: List<String>,
	): SubsonicEnvelope<EmptyBody>

	@GET("rest/updatePlaylist.view")
	suspend fun updatePlaylist(
		@Query("playlistId") playlistId: String,
		@Query("songIdToAdd") songIdToAdd: List<String>,
		/** Positions, not ids — they shift as soon as one is removed. */
		@Query("songIndexToRemove") songIndexToRemove: List<Int>,
	): SubsonicEnvelope<EmptyBody>

	@GET("rest/deletePlaylist.view")
	suspend fun deletePlaylist(@Query("id") id: String): SubsonicEnvelope<EmptyBody>

	// ── Starring ────────────────────────────────────────────────────────
	//
	// The parameter name selects the kind: id for songs, albumId, artistId.

	@GET("rest/star.view")
	suspend fun star(
		@Query("id") songId: String?,
		@Query("albumId") albumId: String?,
		@Query("artistId") artistId: String?,
	): SubsonicEnvelope<EmptyBody>

	@GET("rest/unstar.view")
	suspend fun unstar(
		@Query("id") songId: String?,
		@Query("albumId") albumId: String?,
		@Query("artistId") artistId: String?,
	): SubsonicEnvelope<EmptyBody>

	@GET("rest/getStarred2.view")
	suspend fun getStarred2(): SubsonicEnvelope<Starred2Body>

	// ── Play reporting ──────────────────────────────────────────────────

	/**
	 * `submission=false` is a now-playing notification; `submission=true`
	 * records a completed play, which is what increments the play count and
	 * sets `last_played` — and therefore what makes getRecentSongs non-empty.
	 */
	@GET("rest/scrobble.view")
	suspend fun scrobble(
		@Query("id") id: String,
		@Query("submission") submission: Boolean,
	): SubsonicEnvelope<EmptyBody>

	// ── gaindrive extensions ────────────────────────────────────────────

	@GET("rest/getRecentSongs.view")
	suspend fun getRecentSongs(@Query("size") size: Int): SubsonicEnvelope<GetRecentSongsBody>

	/**
	 * A credential a Cast receiver can fetch one track with, so the URL we hand
	 * it need not carry ours.
	 *
	 * This app holds the Cast control channel itself and builds the receiver's
	 * URLs, and a receiver has no account — so those URLs used to carry
	 * `u`/`t`/`s`, which together are the password: `t` is md5(password + salt)
	 * and `s` is the salt, and a television that has them reads the whole
	 * library as this person until the password changes.
	 *
	 * One token covers the track's stream, its cover art and its subtitles,
	 * expires in twelve hours, and reaches nothing the account could not
	 * already read. It does **not** cover `hls.m3u8`, whose playlist copies the
	 * request's credentials onto every segment.
	 *
	 * Needs no `castRole` and no local network — a client casting for itself is
	 * not asking the server to cast. Older servers do not have it, which is why
	 * every caller treats a failure as "carry on with the ordinary credentials"
	 * rather than as an error.
	 */
	@GET("rest/getCastToken.view")
	suspend fun getCastToken(@Query("id") id: String): SubsonicEnvelope<GetCastTokenBody>

	/**
	 * Puts an album somewhere, under a name, by moving it on the server's disk.
	 *
	 * The server's one mover: `moveAlbum` also renames in place and re-files
	 * under a different artist, since every parameter but the id is optional
	 * and an omitted one means unchanged. This app uses only the promote
	 * shape — out of the account's uploads and into the shared library — so
	 * the destination halves are declared non-null here to say a call site
	 * cannot forget them. Naming a root is admin's alone.
	 *
	 * The move happens on the server's disk, so every id below it changes and
	 * both listings have to be read again.
	 *
	 * The destination halves were briefly optional and both defaults were
	 * guesses that put things in the wrong place — the first `artists` root
	 * declared, which cannot reach a `categories` root at all, and the batch's
	 * own artist name, which for a fetched video is the channel that published
	 * it. An omission now earns error 10 rather than a silent wrong answer.
	 */
	@GET("rest/moveAlbum.view")
	suspend fun moveAlbum(
		@Query("id") id: String,
		/** The destination root, as `getMusicFolders` reports it. */
		@Query("musicFolderId") musicFolderId: String,
		/**
		 * The level under that root: an artist under an `artists` root, a
		 * category under a `categories` one. A name not already there is
		 * created.
		 */
		@Query("folder") folder: String,
	): SubsonicEnvelope<EmptyBody>

	/**
	 * Removes one of the caller's *own* uploaded albums, files and all.
	 *
	 * The server refuses anything whose stored path is not exactly
	 * `<uploads root>/<this account>/<batch>/<artist>/<album>`, which is what
	 * keeps an endpoint that deletes "the folder with this id" from being one
	 * that deletes any folder on the server. Owner only — there is no admin
	 * override, because no account can reach another's uploads to begin with.
	 */
	@GET("rest/deleteUpload.view")
	suspend fun deleteUpload(@Query("id") id: String): SubsonicEnvelope<EmptyBody>

	// ── Fetching a URL into the user's uploads ──────────────────────────
	//
	// OpenSubsonic extension "gaindrive" version 1. All four require the
	// account's upload role, which is why [getUrlHandlers] doubles as the
	// capability probe: it answers error 50 without that role and an error of
	// its own on a server too old to know the endpoint, so one call decides
	// whether this server can be offered at all.

	/**
	 * Which URLs this server can fetch, and whether each handler can produce
	 * audio, video or both.
	 *
	 * An empty list is the server saying the feature is unavailable — no
	 * handler table is configured — and is not an error.
	 */
	@GET("rest/getUrlHandlers.view")
	suspend fun getUrlHandlers(): SubsonicEnvelope<GetUrlHandlersBody>

	/**
	 * Queues a fetch. Returns as soon as the job is queued, not when it is done.
	 *
	 * [artist] and [album] must be null rather than blank when the user typed
	 * nothing: the server distinguishes "not sent" (keep whatever the handler
	 * parsed out of the title) from a value it cannot use, which is error 10.
	 * Retrofit omits a null query parameter entirely, so null is that
	 * distinction.
	 */
	@GET("rest/fetchUrl.view")
	suspend fun fetchUrl(
		@Query("url") url: String,
		/** "audio" or "video". */
		@Query("mode") mode: String,
		@Query("artist") artist: String?,
		@Query("album") album: String?,
	): SubsonicEnvelope<FetchUrlBody>

	/** The caller's own jobs, newest first. Retained briefly after they end. */
	@GET("rest/getFetchJobs.view")
	suspend fun getFetchJobs(): SubsonicEnvelope<GetFetchJobsBody>

	@GET("rest/cancelFetch.view")
	suspend fun cancelFetch(@Query("id") id: String): SubsonicEnvelope<EmptyBody>
}
