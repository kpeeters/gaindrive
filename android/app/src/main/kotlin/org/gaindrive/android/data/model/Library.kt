package org.gaindrive.android.data.model

/**
 * Domain models. Every one carries an [ItemRef] rather than a bare id, so the
 * server an item came from travels with it — which is what makes a merged
 * library, a mixed-server queue and per-item actions possible at all.
 *
 * Separate from the DTOs on purpose: Phase 6's cache will produce these from
 * Room as well as from the network, and retrofitting that split later would
 * touch every screen.
 */

data class Artist(
	val ref: ItemRef,
	val name: String,
	val albumCount: Int,
	val coverArt: ItemRef?,
	val starredAt: String?,
	/**
	 * The servers contributing to this row. One entry normally; several when
	 * artists of the same name have been merged across servers.
	 */
	val sources: List<ServerId> = listOf(ref.server),
) {
	val isStarred: Boolean get() = starredAt != null
}

/** One index bucket from `getArtists`, e.g. "S" or "#". */
data class ArtistIndex(
	val label: String,
	val artists: List<Artist>,
)

data class Album(
	val ref: ItemRef,
	val title: String,
	val artistName: String,
	val artistRef: ItemRef?,
	val songCount: Int,
	val duration: Int,
	val year: Int?,
	val genre: String?,
	val coverArt: ItemRef?,
	val starredAt: String?,
) {
	val isStarred: Boolean get() = starredAt != null
}

data class Song(
	val ref: ItemRef,
	val title: String,
	val artistName: String,
	val albumTitle: String,
	val albumRef: ItemRef?,
	val track: Int?,
	val discNumber: Int?,
	val year: Int?,
	val duration: Int,
	val bitRate: Int,
	val suffix: String?,
	val contentType: String?,
	val sizeBytes: Long,
	val coverArt: ItemRef?,
	val starredAt: String?,
	/** Only populated by the recents query. */
	val lastPlayedAt: String? = null,
) {
	val isStarred: Boolean get() = starredAt != null
}

data class Playlist(
	val ref: ItemRef,
	val name: String,
	val comment: String?,
	val owner: String?,
	val isPublic: Boolean,
	val songCount: Int,
	val duration: Int,
	val songs: List<Song> = emptyList(),
)

/** Biography and outbound links for an artist, from `getArtistInfo2`. */
data class ArtistInfo(
	val biography: String?,
	val wikiUrl: String?,
	val allMusicUrl: String?,
	val lastFmUrl: String?,
	val discogsUrl: String?,
) {
	val isEmpty: Boolean
		get() = biography.isNullOrBlank() && wikiUrl.isNullOrBlank() &&
			allMusicUrl.isNullOrBlank() && lastFmUrl.isNullOrBlank() &&
			discogsUrl.isNullOrBlank()
}

/** Notes and outbound links for an album, from `getAlbumInfo2`. */
data class AlbumNotes(
	val notes: String?,
	val wikiUrl: String?,
	val allMusicUrl: String?,
) {
	val isEmpty: Boolean
		get() = notes.isNullOrBlank() && wikiUrl.isNullOrBlank() && allMusicUrl.isNullOrBlank()
}

/**
 * An album and its tracks — the part of the detail screen that is worth
 * blocking on. The notes are fetched separately (see [AlbumNotes]) because
 * they can be slow and are never essential.
 */
data class AlbumDetail(
	val album: Album,
	val songs: List<Song>,
)

/** Grouped results from a search or from the starred list. */
data class LibrarySelection(
	val artists: List<Artist> = emptyList(),
	val albums: List<Album> = emptyList(),
	val songs: List<Song> = emptyList(),
) {
	val isEmpty: Boolean get() = artists.isEmpty() && albums.isEmpty() && songs.isEmpty()
}

/** What `star`/`unstar` is being applied to; each takes a different parameter. */
enum class StarKind { SONG, ALBUM, ARTIST }
