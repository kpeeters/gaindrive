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
}
