package org.gaindrive.android.data.local

import androidx.room.Dao
import androidx.room.Query
import androidx.room.Upsert

/**
 * Reads and writes of the mirrored library.
 *
 * Every read is scoped to one server, because merging across servers is the
 * repository's job and its rules must not differ between the network path and
 * the stored one.
 */
/** A `(server, id)` pair from a projection, before it becomes an `ItemRef`. */
data class RefRow(val serverId: String, val refId: String)

data class ArtistAlbumRow(val serverId: String, val artistId: String, val albumId: String)

/** [refKey] is already an encoded `ItemRef`, so it matches a cache key directly. */
data class SongSizeRow(val refKey: String, val sizeBytes: Long)

/**
 * One track's place in one collection: which album or playlist, and the track's
 * own cache key.
 *
 * [refKey] and [containerKey] are both already encoded `ItemRef`s, so neither
 * needs rebuilding before being compared against the cache.
 *
 * [isVideo] travels with the row rather than being filtered in SQL because the
 * rule that decides whether a video counts is `Pins.downloadable`, and it has
 * to have one definition — see the comment there.
 */
data class MemberRow(val containerKey: String, val refKey: String, val isVideo: Boolean)

@Dao
interface LibraryDao {

	@Upsert
	suspend fun upsertArtists(rows: List<ArtistEntity>)

	@Upsert
	suspend fun upsertAlbums(rows: List<AlbumEntity>)

	@Upsert
	suspend fun upsertSongs(rows: List<SongEntity>)

	@Upsert
	suspend fun upsertPlaylists(rows: List<PlaylistEntity>)

	@Upsert
	suspend fun upsertPlaylistSongs(rows: List<PlaylistSongEntity>)

	@Query(
		"SELECT * FROM artists WHERE serverId = :server " +
			"AND contentType = :contentType " +
			"ORDER BY indexLabel, name COLLATE NOCASE"
	)
	suspend fun artists(server: String, contentType: String): List<ArtistEntity>

	/**
	 * The slices actually present in the mirror, for the chip row when offline.
	 *
	 * Scoped to the servers asked about, not the whole table: a server that has
	 * been disabled keeps its mirrored rows — it may be enabled again, and
	 * dropping them would mean re-browsing to get them back — so an unscoped
	 * query offers chips for a library that is not currently on offer.
	 */
	@Query("SELECT DISTINCT contentType FROM artists WHERE serverId IN (:servers)")
	suspend fun storedContentTypes(servers: List<String>): List<String>

	@Query("SELECT * FROM artists WHERE serverId = :server AND id = :id")
	suspend fun artist(server: String, id: String): ArtistEntity?

	@Query(
		"SELECT * FROM albums WHERE serverId = :server AND artistId = :artistId " +
			"ORDER BY year, title COLLATE NOCASE"
	)
	suspend fun albumsOfArtist(server: String, artistId: String): List<AlbumEntity>

	@Query("SELECT * FROM albums WHERE serverId = :server AND id = :id")
	suspend fun album(server: String, id: String): AlbumEntity?

	@Query("SELECT * FROM songs WHERE serverId = :server AND id = :id")
	suspend fun song(server: String, id: String): SongEntity?

	@Query(
		"SELECT * FROM songs WHERE serverId = :server AND albumId = :albumId " +
			"ORDER BY discNumber, track, title COLLATE NOCASE"
	)
	suspend fun songsOfAlbum(server: String, albumId: String): List<SongEntity>

	@Query("SELECT * FROM playlists WHERE serverId = :server ORDER BY name COLLATE NOCASE")
	suspend fun playlists(server: String): List<PlaylistEntity>

	@Query("SELECT * FROM playlists WHERE serverId = :server AND id = :id")
	suspend fun playlist(server: String, id: String): PlaylistEntity?

	@Query(
		"SELECT s.* FROM playlist_songs ps " +
			"JOIN songs s ON s.serverId = ps.serverId AND s.id = ps.songId " +
			"WHERE ps.serverId = :server AND ps.playlistId = :playlistId " +
			"ORDER BY ps.position"
	)
	suspend fun songsOfPlaylist(server: String, playlistId: String): List<SongEntity>

	/**
	 * Membership is replaced wholesale rather than diffed: positions shift when
	 * anything is inserted or removed, so a partial update would leave the
	 * stored order disagreeing with the server's.
	 */
	@Query("DELETE FROM playlist_songs WHERE serverId = :server AND playlistId = :playlistId")
	suspend fun clearPlaylistSongs(server: String, playlistId: String)

	@Query(
		"SELECT * FROM artists WHERE serverId = :server AND name LIKE :pattern " +
			"ORDER BY name COLLATE NOCASE LIMIT :limit"
	)
	suspend fun searchArtists(server: String, pattern: String, limit: Int): List<ArtistEntity>

	@Query(
		"SELECT * FROM albums WHERE serverId = :server " +
			"AND (title LIKE :pattern OR artistName LIKE :pattern) " +
			"ORDER BY title COLLATE NOCASE LIMIT :limit"
	)
	suspend fun searchAlbums(server: String, pattern: String, limit: Int): List<AlbumEntity>

	@Query(
		"SELECT * FROM songs WHERE serverId = :server " +
			"AND (title LIKE :pattern OR artistName LIKE :pattern) " +
			"ORDER BY title COLLATE NOCASE LIMIT :limit"
	)
	suspend fun searchSongs(server: String, pattern: String, limit: Int): List<SongEntity>

	// ── What is reachable offline ───────────────────────────────────────────
	//
	// The audio cache knows nothing but encoded song refs, so these compare
	// against `serverId || '/' || id` — the exact string `ItemRef.encode()`
	// produces. Keeping the join in SQL avoids pulling the whole songs table
	// into memory to intersect it.
	//
	// The key list is bounded by the cache's own size cap, so it is short
	// enough to pass as parameters; callers still chunk it against SQLite's
	// variable limit.

	@Query(
		"SELECT DISTINCT serverId, albumId AS refId FROM songs " +
			"WHERE albumId IS NOT NULL AND serverId || '/' || id IN (:keys)"
	)
	suspend fun albumsWithStoredSongs(keys: List<String>): List<RefRow>

	@Query(
		"SELECT DISTINCT serverId, playlistId AS refId FROM playlist_songs " +
			"WHERE serverId || '/' || songId IN (:keys)"
	)
	suspend fun playlistsWithStoredSongs(keys: List<String>): List<RefRow>

	// Whole membership rather than a count, because deciding what counts is
	// `Pins.downloadable`'s job and restating it as an `isVideo = 0` here would
	// be the same rule in two places — an album holding a film would then read
	// as complete in a list and as permanently incomplete on its own screen.
	// Bounded by what has been visited online: there is no whole-library sync.
	//
	// No key parameter, so neither of these needs the chunking the queries
	// above do.

	@Query(
		"SELECT serverId || '/' || albumId AS containerKey, " +
			"serverId || '/' || id AS refKey, isVideo FROM songs " +
			"WHERE albumId IS NOT NULL"
	)
	suspend fun albumMembership(): List<MemberRow>

	@Query(
		"SELECT ps.serverId || '/' || ps.playlistId AS containerKey, " +
			"ps.serverId || '/' || ps.songId AS refKey, s.isVideo AS isVideo " +
			"FROM playlist_songs ps " +
			"JOIN songs s ON s.serverId = ps.serverId AND s.id = ps.songId"
	)
	suspend fun playlistMembership(): List<MemberRow>

	/**
	 * The server's byte size for songs the cache is holding.
	 *
	 * The fallback for judging whether a cached track is complete when the cache
	 * itself cannot say — see `AudioCache.refresh`.
	 */
	@Query(
		"SELECT serverId || '/' || id AS refKey, sizeBytes FROM songs " +
			"WHERE serverId || '/' || id IN (:keys)"
	)
	suspend fun songSizes(keys: List<String>): List<SongSizeRow>

	/**
	 * Which of these refs name videos.
	 *
	 * One query rather than a `song()` per ref, because the caller is
	 * `PinRepository.applyProtection` and a playlist pin can cover hundreds.
	 * It needs the answer to build the right cache key: a video played for its
	 * soundtrack substitutes a real container for `AudioQuality.ORIGINAL` (see
	 * `AudioQuality.forVideoAudio`), so protecting it under the unsubstituted
	 * key would protect bytes that are not there while the ones that are stay
	 * evictable.
	 */
	@Query(
		"SELECT serverId || '/' || id AS refKey FROM songs " +
			"WHERE isVideo != 0 AND serverId || '/' || id IN (:keys)"
	)
	suspend fun videoRefKeys(keys: List<String>): List<String>

	/**
	 * One row per album, not distinct: the caller counts them to say how many
	 * albums an artist actually has on the device.
	 */
	@Query(
		"SELECT serverId, artistId, id AS albumId FROM albums " +
			"WHERE artistId IS NOT NULL AND serverId || '/' || id IN (:keys)"
	)
	suspend fun artistAlbumsOf(keys: List<String>): List<ArtistAlbumRow>

	// ── Removing a server ───────────────────────────────────────────────────

	@Query("DELETE FROM artists WHERE serverId = :server")
	suspend fun deleteArtists(server: String)

	@Query("DELETE FROM albums WHERE serverId = :server")
	suspend fun deleteAlbums(server: String)

	@Query("DELETE FROM songs WHERE serverId = :server")
	suspend fun deleteSongs(server: String)

	@Query("DELETE FROM playlists WHERE serverId = :server")
	suspend fun deletePlaylists(server: String)

	@Query("DELETE FROM playlist_songs WHERE serverId = :server")
	suspend fun deletePlaylistSongs(server: String)
}
