package org.gaindrive.android.data

import androidx.media3.common.MimeTypes
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.flow.first
import kotlinx.coroutines.withContext
import org.gaindrive.android.data.cache.AudioCache
import org.gaindrive.android.data.cache.CacheKeys
import org.gaindrive.android.data.model.AudioFormat
import org.gaindrive.android.data.model.AudioQuality
import org.gaindrive.android.data.model.ItemRef
import org.gaindrive.android.net.SubsonicClientFactory
import javax.inject.Inject
import javax.inject.Singleton

/**
 * Where to fetch a track's audio from, at what quality, and under which cache
 * key.
 *
 * The three travel together on purpose. A URL naming one quality paired with a
 * key naming another stores bytes that will later be served to a request
 * expecting something else, and nothing downstream can detect it. Producing
 * them in one place is what makes that impossible.
 */
data class StreamTarget(
	val url: String,
	val quality: AudioQuality,
	val cacheKey: String,
	val mimeType: String?,
)

/**
 * Where to fetch a video from. No quality and no cache key, because both of
 * those are audio concepts here: the server chooses the video tier itself, and
 * video deliberately never enters the byte cache.
 */
data class VideoTarget(
	val url: String,
	val mimeType: String?,
	/** True when [url] is an HLS playlist rather than a progressive stream. */
	val isHls: Boolean,
)

/**
 * The single builder of stream URLs, used by both the download queue and the
 * player.
 *
 * Deliberately not reached through `LibraryRepository`: that class writes the
 * mirror `PinRepository` reads, and a dependency back would close the loop —
 * the same reasoning that kept `PinRepository` from using `CoverUrls`.
 */
@Singleton
class StreamUrls @Inject constructor(
	private val registry: ServerRegistry,
	private val clients: SubsonicClientFactory,
	private val settings: SettingsStore,
	private val limits: AccountLimits,
	private val audioCache: AudioCache,
) {

	/**
	 * Always the current setting: pinning is the user asking for this track at
	 * the quality they have chosen, so an older copy does not satisfy it.
	 */
	suspend fun forDownload(ref: ItemRef, audioOnlyVideo: Boolean = false): StreamTarget? =
		build(ref, settings.audioQuality.first(), audioOnlyVideo)

	/**
	 * Prefers a quality already held in full, so a library downloaded at an
	 * earlier setting keeps playing from disk instead of being re-fetched.
	 *
	 * The URL is built for the held quality rather than the current one: if the
	 * copy turns out to need topping up, the bytes that arrive have to match the
	 * bytes already there.
	 */
	suspend fun forPlayback(ref: ItemRef, audioOnlyVideo: Boolean = false): StreamTarget? {
		val preferred = settings.audioQuality.first().let {
			if (audioOnlyVideo) it.forVideoAudio() else it
		}
		// Off the main thread: this walks the cache index, and onAddMediaItems
		// runs on the player's thread.
		val held = withContext(Dispatchers.IO) {
			audioCache.heldTagOf(ref.encode(), preferred)
		}?.let(AudioQuality::parse)
		return build(ref, held ?: preferred, audioOnlyVideo)
	}

	/**
	 * The file as it stands, for a Cast receiver that fetches from the server
	 * itself. See `SettingsStore.castOriginal` for why that route alone.
	 *
	 * Deliberately does **not** prefer a quality already held, the way
	 * [forPlayback] does: a copy stored at the streaming quality is the answer
	 * to a different question here, and preferring it would defeat the setting
	 * on precisely the tracks the user listens to most.
	 *
	 * The account ceiling still applies — `AudioQuality.cappedBy` turns a
	 * request for the original into mp3 at the cap — because the server would
	 * enforce it whatever was asked for. So "original" means "as far as the
	 * account allows", which the track info dialog's `Sent` row makes visible.
	 *
	 * The returned `cacheKey` names bytes nobody will store: the receiver
	 * fetches this URL, not the phone. It is carried only because
	 * [StreamTarget] travels as one value, and the direct cast path returns
	 * without ever reading it.
	 */
	suspend fun forCastOriginal(ref: ItemRef): StreamTarget? =
		build(ref, AudioQuality.ORIGINAL)

	/**
	 * Where to fetch a video from, and how it will have to be seeked.
	 *
	 * Two parameters are conspicuously absent, and both omissions are
	 * load-bearing:
	 *
	 *  - **`format`** is validated against the *audio* target table, so a video
	 *    container name is rejected outright and an audio one asks the server
	 *    for the soundtrack alone. That second behaviour is now a feature —
	 *    `SettingsStore.videoAudioOnly` — but it is reached by resolving the
	 *    item through the *audio* path instead, never from here. This builder
	 *    is for someone who wants the picture, and a format here would take it
	 *    away. `web/app.js` splits the same two cases the same way.
	 *  - **`maxBitRate`** sets `constrained` server-side, which forces the
	 *    re-encode tier and demotes a file that could have been served straight
	 *    off disk. The account ceiling is applied by the server regardless of
	 *    what is asked for, so sending one buys nothing and costs the tier.
	 *
	 * [nativeSeek] comes from the entry the caller already has. False means the
	 * server can only re-encode this file, which is chunked with no
	 * `Content-Length` and no `Range` — unseekable as a progressive stream, so
	 * it is played as HLS, where seeking is picking a segment.
	 */
	suspend fun forVideo(ref: ItemRef, nativeSeek: Boolean): VideoTarget? =
		withContext(Dispatchers.IO) {
			val config = registry.get(ref.server) ?: return@withContext null
			val client = clients.clientFor(config)
			val params = mapOf("id" to ref.id)

			if (nativeSeek) {
				VideoTarget(
					url = client.url("stream", params),
					// No declared type: sniffing is the only honest answer, the
					// same argument AudioFormat.ORIGINAL makes. The remux tier
					// turns an .mkv into MP4, and a VP9/Opus .mkv is served
					// relabelled video/webm — so the entry's own contentType is
					// wrong in exactly the cases that matter.
					mimeType = null,
					isHls = false,
				)
			} else {
				VideoTarget(
					// The playlist copies this request's auth parameters onto
					// every segment URL, so the player needs no context from
					// here to fetch them.
					url = client.url("hls.m3u8", params, suffix = ""),
					mimeType = MimeTypes.APPLICATION_M3U8,
					isHls = true,
				)
			}
		}

	/**
	 * [audioOnlyVideo] says [ref] names a video and only its soundtrack is
	 * wanted. It changes exactly one thing — [AudioQuality.ORIGINAL] cannot be
	 * expressed for a video, since it is spelled by sending no `format` and
	 * that fetches the film — but it has to be threaded all the way here
	 * because the cache key is derived from the same quality value.
	 */
	private suspend fun build(
		ref: ItemRef,
		wanted: AudioQuality,
		audioOnlyVideo: Boolean = false,
	): StreamTarget? =
		withContext(Dispatchers.IO) {
			val config = registry.get(ref.server) ?: return@withContext null
			// The account ceiling is applied here, where this track's own server
			// is in hand. A queue may span servers, so there is no single
			// "current" cap to read.
			val quality = (if (audioOnlyVideo) wanted.forVideoAudio() else wanted)
				.cappedBy(limits.capFor(config))

			val params = buildMap {
				put("id", ref.id)
				// Omitted entirely for the original: the server serves the file
				// directly, with no ffmpeg involved at all.
				if (quality.format != AudioFormat.ORIGINAL) {
					put("format", quality.format.param)
					put("maxBitRate", quality.bitRate.toString())
				}
			}

			StreamTarget(
				url = clients.clientFor(config).url("stream", params),
				quality = quality,
				cacheKey = CacheKeys.of(ref, quality),
				mimeType = quality.format.mime,
			)
		}
}
