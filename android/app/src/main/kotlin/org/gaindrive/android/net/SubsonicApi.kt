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

	@GET("rest/getArtists.view")
	suspend fun getArtists(
		/** gaindrive extension: restricts the list to the user's own uploads. */
		@Query("personal") personal: String?,
	): SubsonicEnvelope<GetArtistsBody>

	@GET("rest/getArtist.view")
	suspend fun getArtist(@Query("id") id: String): SubsonicEnvelope<GetArtistBody>

	@GET("rest/getAlbum.view")
	suspend fun getAlbum(@Query("id") id: String): SubsonicEnvelope<GetAlbumBody>

	@GET("rest/getArtistInfo2.view")
	suspend fun getArtistInfo2(@Query("id") id: String): SubsonicEnvelope<GetArtistInfoBody>

	@GET("rest/getAlbumInfo2.view")
	suspend fun getAlbumInfo2(@Query("id") id: String): SubsonicEnvelope<GetAlbumInfoBody>

	// ── Searching ───────────────────────────────────────────────────────

	@GET("rest/search3.view")
	suspend fun search3(
		@Query("query") query: String,
		@Query("artistCount") artistCount: Int,
		@Query("albumCount") albumCount: Int,
		@Query("songCount") songCount: Int,
	): SubsonicEnvelope<Search3Body>

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

	// ── gaindrive extensions ────────────────────────────────────────────

	@GET("rest/getRecentSongs.view")
	suspend fun getRecentSongs(@Query("size") size: Int): SubsonicEnvelope<GetRecentSongsBody>
}
