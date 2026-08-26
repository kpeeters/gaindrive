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

	// ── Searching ───────────────────────────────────────────────────────

	@GET("rest/search3.view")
	suspend fun search3(
		@Query("query") query: String,
		@Query("artistCount") artistCount: Int,
		@Query("albumCount") albumCount: Int,
		@Query("songCount") songCount: Int,
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
	 * Moves an album out of the account's uploads and into the shared library.
	 *
	 * Admin only, and the id must be an album whose stored path has the exact
	 * shape `<uploads root>/<user>/<batch>/<artist>/<album>` — anything else is
	 * refused rather than guessed at. The move happens on the server's disk, so
	 * every id below it changes and both listings have to be read again.
	 */
	@GET("rest/promoteAlbum.view")
	suspend fun promoteAlbum(
		@Query("id") id: String,
		/**
		 * The destination root, as `getMusicFolders` reports it. Null falls back
		 * to the server's own guess — the first `artists` root declared — which
		 * cannot reach a `categories` root at all.
		 */
		@Query("musicFolderId") musicFolderId: String?,
		/**
		 * The level under that root: an artist under an `artists` root, a
		 * category under a `categories` one. Null keeps the batch's own artist
		 * name. A name not already there is created.
		 */
		@Query("folder") folder: String?,
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
	// OpenSubsonic extension "gaindrive" version 2. All four require the
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
