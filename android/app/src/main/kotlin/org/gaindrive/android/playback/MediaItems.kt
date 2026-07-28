package org.gaindrive.android.playback

import android.net.Uri
import androidx.media3.common.MediaItem
import androidx.media3.common.MediaMetadata
import org.gaindrive.android.data.model.ItemRef
import org.gaindrive.android.data.model.Song

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
	val artworkUrl: String?,
)

fun MediaItem.toNowPlaying(): NowPlaying = NowPlaying(
	ref = itemRef(),
	title = mediaMetadata.title?.toString().orEmpty(),
	artist = mediaMetadata.artist?.toString().orEmpty(),
	album = mediaMetadata.albumTitle?.toString().orEmpty(),
	artworkUrl = mediaMetadata.artworkUri?.toString(),
)
