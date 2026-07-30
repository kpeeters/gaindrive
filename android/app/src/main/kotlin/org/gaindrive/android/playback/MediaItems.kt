package org.gaindrive.android.playback

import android.net.Uri
import androidx.core.os.bundleOf
import androidx.media3.common.MediaItem
import androidx.media3.common.MediaMetadata
import org.gaindrive.android.data.model.ItemRef
import org.gaindrive.android.data.model.Song

/**
 * The song's album, carried in the metadata extras so the player can offer
 * "go to album" without a lookup. Media3 has an album *title* field but no
 * album id, hence the extra.
 */
private const val KEY_ALBUM_REF = "org.gaindrive.albumRef"

/**
 * Song ↔ MediaItem. The `mediaId` carries the encoded [ItemRef], because it is
 * the only context Media3 hands back on notification actions and session
 * restore — a bare song id there would be ambiguous the moment a second server
 * is configured.
 *
 * Items leave here with **no URI**. The service resolves that in
 * `onAddMediaItems`, so the stream policy lives in one place and the UI never
 * has to know how a stream URL is built.
 */
fun Song.toMediaItem(artworkUrl: String?): MediaItem {
	val metadata = MediaMetadata.Builder()
		.setTitle(title)
		.setArtist(artistName)
		.setAlbumTitle(albumTitle)
		.setIsBrowsable(false)
		.setIsPlayable(true)
		.apply { artworkUrl?.let { setArtworkUri(Uri.parse(it)) } }
		.apply { albumRef?.let { setExtras(bundleOf(KEY_ALBUM_REF to it.encode())) } }
		.build()

	return MediaItem.Builder()
		.setMediaId(ref.encode())
		.setMediaMetadata(metadata)
		.build()
}

/** The reference a [MediaItem] was built from, or null if its id is malformed. */
fun MediaItem.itemRef(): ItemRef? = ItemRef.decode(mediaId)

/** What the player UI needs to render one queue entry. */
data class NowPlaying(
	val ref: ItemRef?,
	val title: String,
	val artist: String,
	val album: String,
	/**
	 * Null when the server gave the song no album id, or for a queue the system
	 * restored from a bare media id after process death — the player then simply
	 * offers no way through to the album.
	 */
	val albumRef: ItemRef?,
	val artworkUrl: String?,
)

fun MediaItem.toNowPlaying(): NowPlaying = NowPlaying(
	ref = itemRef(),
	title = mediaMetadata.title?.toString().orEmpty(),
	artist = mediaMetadata.artist?.toString().orEmpty(),
	album = mediaMetadata.albumTitle?.toString().orEmpty(),
	albumRef = mediaMetadata.extras?.getString(KEY_ALBUM_REF)?.let { ItemRef.decode(it) },
	artworkUrl = mediaMetadata.artworkUri?.toString(),
)
