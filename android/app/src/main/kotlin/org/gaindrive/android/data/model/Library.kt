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
	 * Every (server, id) pair this row stands for. One entry normally; several
	 * when artists of the same name have been merged across servers, in which
	 * case [ref] is the first contributor in registry order.
	 *
	 * Ids, not just server ids: opening a merged artist has to ask each server
	 * for *its* artist, and the ids differ.
	 */
	val refs: List<ItemRef> = listOf(ref),
) {
	val isStarred: Boolean get() = starredAt != null

	/** The servers contributing to this row, for the badges. */
	val sources: List<ServerId> get() = refs.map { it.server }
}

/**
 * One configured library root, from `getMusicFolders`.
 *
 * [contentType] is a gaindrive extension and is null everywhere else — which is
 * the whole of what tells a server that knows about kinds of root from one that
 * only has folders. See `data/browse/LibraryRoots.kt`.
 */
data class MusicRoot(
	val id: String,
	val name: String,
	val contentType: String?,
)

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
	/**
	 * Every server holding this album. One entry unless duplicates have been
	 * collapsed, in which case [ref] is the copy that won — the one highest in
	 * registry order — and the rest are kept so the row can still say who else
	 * has it.
	 */
	val refs: List<ItemRef> = listOf(ref),
) {
	val isStarred: Boolean get() = starredAt != null

	/** The servers holding this album, for the badges. */
	val sources: List<ServerId> get() = refs.map { it.server }
}

data class Song(
	val ref: ItemRef,
	val title: String,
	/** Who made this track, which on a compilation is not [albumArtistName]. */
	val artistName: String,
	/**
	 * Who the album is by. Blank when the server did not say — an older one, or
	 * a listing built offline before this was mirrored — which is why
	 * [differingArtist] insists on having it before drawing anything.
	 */
	val albumArtistName: String = "",
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
	val isVideo: Boolean = false,
	/**
	 * Whether the stream this song would produce can be seeked by byte range.
	 * False for a video the server can only re-encode on the fly, which is what
	 * sends playback to `hls.m3u8` instead. Meaningless for audio.
	 */
	val nativeSeek: Boolean = false,
	/**
	 * The season an episode belongs to, or null. [discNumber] holds the same
	 * number and is what orders and groups tracks; this only decides whether
	 * the group is headed "Series 2" or "Disc 2".
	 */
	val season: Int? = null,
	/**
	 * What the server will actually send if it has to convert this, or null when
	 * it will send the file as it stands.
	 *
	 * Only casting reads it, and it is the reason casting does not have to work
	 * out the tier for itself: the receiver picks its decode pipeline from the
	 * declared type, and for a video the *source* `contentType` is wrong in
	 * exactly the common case — an H.264/AAC `.mkv` is remuxed and arrives as
	 * `video/mp4`, not as `video/x-matroska`.
	 */
	val transcodedContentType: String? = null,
	/** Video frame size, when the server could probe it. */
	val width: Int? = null,
	val height: Int? = null,
) {
	val isStarred: Boolean get() = starredAt != null

	/**
	 * The track's own artist when the album is not by them, and null otherwise
	 * — so a row can draw it without deciding anything.
	 *
	 * An exact comparison on purpose. The server already answered the hard
	 * half: it sends the folder's spelling in [artistName] whenever the file's
	 * tag is merely a different way of writing the same name, so "AC/DC"
	 * against a folder called "AC-DC" never reaches here as a difference.
	 */
	val differingArtist: String?
		get() = artistName.takeIf {
			it.isNotBlank() && albumArtistName.isNotBlank() && it != albumArtistName
		}

	/** The frame's aspect, or null when the dimensions are unknown. */
	val aspectRatio: Float?
		get() = if (width != null && height != null && width > 0 && height > 0) {
			width.toFloat() / height.toFloat()
		} else {
			null
		}
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
	/**
	 * Chapter markers whose titles matched, which only a search fills in.
	 *
	 * Defaulted because the starred list shares this type and never has any,
	 * and because nothing mirrors them: a marker has no id Room could key on,
	 * so the offline branch and the stored fallback both leave this empty and
	 * the Chapters section simply does not appear.
	 */
	val chapters: List<ChapterHit> = emptyList(),
) {
	val isEmpty: Boolean
		get() = artists.isEmpty() && albums.isEmpty() && songs.isEmpty() && chapters.isEmpty()
}

/** What `star`/`unstar` is being applied to; each takes a different parameter. */
enum class StarKind { SONG, ALBUM, ARTIST }
