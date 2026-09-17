package org.gaindrive.android.playback

import android.net.Uri
import android.os.Bundle
import androidx.core.os.bundleOf
import androidx.media3.common.MediaItem
import androidx.media3.common.MediaMetadata
import org.gaindrive.android.data.model.AudioQuality
import org.gaindrive.android.data.model.ItemRef
import org.gaindrive.android.data.model.Song

/**
 * The song's album, carried in the metadata extras so the player can offer
 * "go to album" without a lookup. Media3 has an album *title* field but no
 * album id, hence the extra.
 */
private const val KEY_ALBUM_REF = "org.gaindrive.albumRef"

/**
 * The source file's own MIME type, as the server reported it.
 *
 * Only casting reads it, and only when nothing else answers: for audio, when
 * the chosen quality is the original file, and for video, when the server will
 * send the file as it stands. A Cast receiver picks its decode pipeline from
 * `contentType`, and a wrong guess surfaces as a decode error minutes into
 * playback rather than as a refusal to start. Every transcoded audio quality
 * knows its own MIME already, and for video [KEY_TRANSCODED_TYPE] does.
 */
private const val KEY_CONTENT_TYPE = "org.gaindrive.contentType"

/**
 * What the server will send if it has to convert this one, and absent when it
 * will send the file as it stands.
 *
 * Also read only by casting, and it is what saves the cast path from working
 * out the server's tier for itself: an H.264/AAC `.mkv` is remuxed and reaches
 * the receiver as `video/mp4`, so [KEY_CONTENT_TYPE] — the *source* container —
 * is wrong in exactly the case that matters most. Deriving it here instead
 * would be a second copy of `video_direct_playable()`, in another language.
 */
private const val KEY_TRANSCODED_TYPE = "org.gaindrive.transcodedContentType"

/**
 * Whether this entry is a video, and if so what the server said about seeking
 * it and how big its frame is.
 *
 * Carried on the item because three separate decisions need it before anything
 * has been fetched: which stream URL to build, whether the source may go
 * through the byte cache, and whether the UI should offer a picture at all.
 * The seek flag rides along so the service does not have to fetch the song
 * again to learn something the browse response already told it.
 */
private const val KEY_IS_VIDEO = "org.gaindrive.isVideo"
private const val KEY_NATIVE_SEEK = "org.gaindrive.nativeSeek"
private const val KEY_ASPECT = "org.gaindrive.aspect"

/**
 * Set when this item is a video resolved for its soundtrack alone.
 *
 * It is deliberately *not* [KEY_IS_VIDEO]. Such an item is an audio stream in
 * every way that matters downstream — it goes through the byte cache, it draws
 * no surface, it can be downloaded — so leaving the video flag off is what
 * makes all of that fall out with no consumer needing to know the setting
 * exists.
 *
 * One consumer does need to know, and it is the reason this extra exists at
 * all: casting. [CastSource] must not offer the receiver the "original file"
 * quality for one of these, because the original file is the film.
 */
private const val KEY_AUDIO_ONLY_VIDEO = "org.gaindrive.audioOnlyVideo"

/**
 * The quality the stream URL was actually built for, written by the service
 * when it resolves the item and read by the track info dialog.
 *
 * It has to be recorded rather than worked out again, because the answer
 * depends on three things that all move: the quality setting, the account's
 * bitrate ceiling, and which quality the byte cache already holds in full. Two
 * of those can change while a track is playing, so a second computation would
 * describe what the *next* track will get rather than what this one did.
 *
 * Stored as [AudioQuality.tag], which is already the short, stable spelling the
 * cache keys on, with [AudioQuality.parse] as its inverse.
 */
private const val KEY_QUALITY = "org.gaindrive.quality"

/**
 * The size the server is asked to scale notification artwork to, and the size
 * `CoilBitmapLoader` then asks Coil for. Here rather than in `PlayerConnection`
 * because the two have to agree: a different number on the loading side would
 * be a second cache entry for a picture already held.
 */
const val ARTWORK_PX = 512

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
		// The server's own figure. ExoPlayer discovers duration for itself, but a
		// Cast receiver benefits from being told before it has fetched a byte,
		// and it is what the queue shows for tracks that have not played yet.
		.setDurationMs(duration.toLong() * 1000)
		.setIsBrowsable(false)
		.setIsPlayable(true)
		.apply { artworkUrl?.let { setArtworkUri(Uri.parse(it)) } }
		.setExtras(
			bundleOf(
				KEY_ALBUM_REF to albumRef?.encode(),
				KEY_CONTENT_TYPE to contentType,
				KEY_TRANSCODED_TYPE to transcodedContentType,
				KEY_IS_VIDEO to isVideo,
				KEY_NATIVE_SEEK to nativeSeek,
				// 0 rather than null: a Bundle float has no absent value, and
				// the reader treats anything non-positive as "not known yet",
				// which is also what an unprobed video gives.
				KEY_ASPECT to (aspectRatio ?: 0f),
			)
		)
		.build()

	return MediaItem.Builder()
		.setMediaId(ref.encode())
		.setMediaMetadata(metadata)
		.build()
}

/** The reference a [MediaItem] was built from, or null if its id is malformed. */
fun MediaItem.itemRef(): ItemRef? = ItemRef.decode(mediaId)

/**
 * The source file's MIME type, if this item still carries it. Null for a queue
 * the system restored from bare media ids after process death, where casting
 * falls back to letting the receiver sniff.
 */
fun MediaItem.sourceContentType(): String? =
	mediaMetadata.extras?.getString(KEY_CONTENT_TYPE)

/**
 * Everything the cast path needs from a queue entry that it would otherwise
 * have to go and fetch again.
 *
 * It travels as one value rather than four arguments because the four are only
 * meaningful together: whether this is a video decides which stream builder to
 * use, whether it seeks natively decides whether it can be cast at all, and the
 * two MIME types are a pair — the second overrides the first exactly when the
 * server is going to convert.
 */
data class CastSource(
	val isVideo: Boolean,
	val nativeSeek: Boolean,
	val sourceMime: String?,
	val transcodedMime: String?,
	/** See [KEY_AUDIO_ONLY_VIDEO]. Never true at the same time as [isVideo]. */
	val audioOnlyVideo: Boolean = false,
)

/**
 * Reads [CastSource] off this item.
 *
 * A queue the system restored from bare media ids after process death carries
 * no extras, and so reports no video, no native seek and no types. For casting
 * that is the safe direction and lands such an item on the same path as a file
 * the receiver could not have played anyway — the service re-derives `isVideo`
 * from the mirror for *local* playback, which is where it matters.
 */
fun MediaItem.castSource(): CastSource = CastSource(
	isVideo = isVideo(),
	nativeSeek = nativeSeek(),
	sourceMime = sourceContentType(),
	transcodedMime = mediaMetadata.extras?.getString(KEY_TRANSCODED_TYPE),
	audioOnlyVideo = isAudioOnlyVideo(),
)

/** Whether this item is a video resolved as audio; see [KEY_AUDIO_ONLY_VIDEO]. */
fun MediaItem.isAudioOnlyVideo(): Boolean =
	mediaMetadata.extras?.getBoolean(KEY_AUDIO_ONLY_VIDEO) == true

/**
 * The same item, now saying it is a video being played as audio.
 *
 * Copied rather than mutated, for the reason [markedAsVideo] gives: the
 * metadata hands out its own Bundle, and writing into it would edit an item
 * other code may already be holding.
 */
fun MediaItem.markedAsAudioOnlyVideo(): MediaItem {
	val extras = Bundle(mediaMetadata.extras ?: Bundle()).apply {
		putBoolean(KEY_AUDIO_ONLY_VIDEO, true)
		// Cleared as well as set. An item re-resolved because the setting was
		// turned off must not go on claiming it is a video, and the reverse
		// re-resolve goes through markedAsVideo().
		putBoolean(KEY_IS_VIDEO, false)
	}
	return buildUpon()
		.setMediaMetadata(mediaMetadata.buildUpon().setExtras(extras).build())
		.build()
}

/**
 * Whether this item is a video, or null when the item carries no extras at all
 * — a queue the system restored from bare media ids after process death.
 *
 * Null is not false, and the difference matters: resolving a video as audio
 * gets the soundtrack alone, so the caller has to go and find out rather than
 * assume. [PlaybackService] asks the local mirror.
 */
fun MediaItem.isVideoOrNull(): Boolean? =
	// containsKey rather than the Bundle's own default: a queue saved by a
	// build that predates video has extras, just not this one, and reading
	// `false` there would call a film audio instead of going to look.
	mediaMetadata.extras
		?.takeIf { it.containsKey(KEY_IS_VIDEO) }
		?.getBoolean(KEY_IS_VIDEO)

/** False for anything whose video-ness is not known here; see [isVideoOrNull]. */
fun MediaItem.isVideo(): Boolean = isVideoOrNull() == true

fun MediaItem.nativeSeek(): Boolean =
	mediaMetadata.extras?.getBoolean(KEY_NATIVE_SEEK) == true

/** The frame's aspect as the server reported it, or null if it never did. */
fun MediaItem.aspectRatio(): Float? =
	mediaMetadata.extras?.getFloat(KEY_ASPECT)?.takeIf { it > 0f }

/**
 * The quality this item's URL was built for, or null for one that has not been
 * resolved yet, for a video (which has no such choice), or for a queue the
 * system restored from bare media ids. The info dialog omits the row rather
 * than guessing.
 */
fun MediaItem.quality(): AudioQuality? =
	mediaMetadata.extras?.getString(KEY_QUALITY)?.let(AudioQuality::parse)

/**
 * The same item, now recording the quality it was resolved at.
 *
 * Copied rather than mutated, for the reason [markedAsVideo] gives: the
 * metadata hands out its own Bundle, and writing into it would edit an item
 * other code may already be holding.
 */
fun MediaItem.withQuality(quality: AudioQuality): MediaItem {
	val extras = Bundle(mediaMetadata.extras ?: Bundle()).apply {
		putString(KEY_QUALITY, quality.tag)
	}
	return buildUpon()
		.setMediaMetadata(mediaMetadata.buildUpon().setExtras(extras).build())
		.build()
}

/**
 * The same item, now saying it is a video.
 *
 * For the queue the system restored from bare media ids: the service works out
 * what such an item is by asking the mirror, and everything downstream — which
 * data source loads it, whether the UI offers a picture — reads the flag off
 * the item rather than repeating that lookup. Without this the answer would be
 * found once and then thrown away.
 */
fun MediaItem.markedAsVideo(): MediaItem {
	// Copied rather than mutated: MediaMetadata hands out its own Bundle, and
	// writing into it would edit an item other code may already be holding.
	val extras = Bundle(mediaMetadata.extras ?: Bundle()).apply {
		putBoolean(KEY_IS_VIDEO, true)
		// The other half of the pair, for an item re-resolved because
		// `videoAudioOnly` was turned off while it was queued.
		putBoolean(KEY_AUDIO_ONLY_VIDEO, false)
	}
	return buildUpon()
		.setMediaMetadata(mediaMetadata.buildUpon().setExtras(extras).build())
		.build()
}

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
	/** Drives whether the UI offers a picture; see [isVideoOrNull]. */
	val isVideo: Boolean = false,
	/**
	 * For a video, whether it can be cast: the receiver is offered only the tier
	 * the server can hand over as a seekable MP4. Meaningless for audio, which
	 * is always castable.
	 */
	val nativeSeek: Boolean = false,
	/** The server's figure, used to shape the surface before the first frame. */
	val aspectRatio: Float? = null,
	/** What the server was asked to send; see [KEY_QUALITY]. Null for video. */
	val quality: AudioQuality? = null,
)

fun MediaItem.toNowPlaying(): NowPlaying = NowPlaying(
	ref = itemRef(),
	title = mediaMetadata.title?.toString().orEmpty(),
	artist = mediaMetadata.artist?.toString().orEmpty(),
	album = mediaMetadata.albumTitle?.toString().orEmpty(),
	albumRef = mediaMetadata.extras?.getString(KEY_ALBUM_REF)?.let { ItemRef.decode(it) },
	artworkUrl = mediaMetadata.artworkUri?.toString(),
	isVideo = isVideo(),
	nativeSeek = nativeSeek(),
	aspectRatio = aspectRatio(),
	quality = quality(),
)
