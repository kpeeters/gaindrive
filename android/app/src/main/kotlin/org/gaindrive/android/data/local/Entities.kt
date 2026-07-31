package org.gaindrive.android.data.local

import androidx.room.Entity

/**
 * The stored mirror of the browse metadata, plus the pin list.
 *
 * Every music row is keyed on `(serverId, id)`, never on the bare Subsonic id:
 * two servers will both have an artist with id 42, and the composite identity
 * rule that holds everywhere else in the app has to hold in storage too.
 *
 * References to other items are stored as bare ids rather than encoded refs,
 * because the server is already the row's own [serverId] — an album's artist
 * and cover art always belong to the server that issued the album.
 */

@Entity(tableName = "artists", primaryKeys = ["serverId", "id"])
data class ArtistEntity(
	val serverId: String,
	val id: String,
	val name: String,
	val albumCount: Int,
	val coverArtId: String?,
	val starredAt: String?,
	/** The `getArtists` index bucket, kept so the letter rail works offline. */
	val indexLabel: String,
)

@Entity(tableName = "albums", primaryKeys = ["serverId", "id"])
data class AlbumEntity(
	val serverId: String,
	val id: String,
	val title: String,
	val artistName: String,
	val artistId: String?,
	val songCount: Int,
	val duration: Int,
	val year: Int?,
	val genre: String?,
	val coverArtId: String?,
	val starredAt: String?,
)

@Entity(tableName = "songs", primaryKeys = ["serverId", "id"])
data class SongEntity(
	val serverId: String,
	val id: String,
	val title: String,
	val artistName: String,
	val albumTitle: String,
	val albumId: String?,
	val track: Int?,
	val discNumber: Int?,
	val year: Int?,
	val duration: Int,
	val bitRate: Int,
	val suffix: String?,
	val contentType: String?,
	val sizeBytes: Long,
	val coverArtId: String?,
	val starredAt: String?,
)

@Entity(tableName = "playlists", primaryKeys = ["serverId", "id"])
data class PlaylistEntity(
	val serverId: String,
	val id: String,
	val name: String,
	val comment: String?,
	val owner: String?,
	val isPublic: Boolean,
	val songCount: Int,
	val duration: Int,
)

/**
 * Playlist membership, ordered.
 *
 * Position is part of the key rather than a plain column: a playlist may hold
 * the same track twice, so `(playlist, song)` is not unique.
 */
@Entity(tableName = "playlist_songs", primaryKeys = ["serverId", "playlistId", "position"])
data class PlaylistSongEntity(
	val serverId: String,
	val playlistId: String,
	val position: Int,
	val songId: String,
)

/**
 * What the user asked to keep.
 *
 * Records the *intent* — this album, that playlist — rather than the songs it
 * currently expands to. A playlist gaining a track should extend the pin, which
 * it only can if the pin is on the playlist.
 */
@Entity(tableName = "pins")
data class PinEntity(
	@androidx.room.PrimaryKey
	/** An encoded `ItemRef`; also the audio cache key when [kind] is SONG. */
	val refKey: String,
	val kind: String,
)
