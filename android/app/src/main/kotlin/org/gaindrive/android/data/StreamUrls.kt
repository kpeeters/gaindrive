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
 * The query a progressive video request carries: the id, and the containers we
 * told the server we demux ourselves.
 *
 * Top-level and `internal` for the reason `CastUrls.paced()` is - it can then
 * be exercised with no Hilt and no `android.util`, which matters here because
 * the failure it guards against is silent and happens on a television. The
 * test that asserts an empty set yields exactly `{id}` is the one standing
 * between a refactor and every cast of an `.mkv` failing.
 *
 * Sorted rather than joined in set order: these end up in log lines and in
 * OkHttp's cache key, and a `Set`'s iteration order is not a promise, so one
 * request would otherwise be able to build two different URLs.
 */
internal fun videoStreamParams(id: String, containers: Set<String>): Map<String, String> =
	buildMap {
		put("id", id)
		if (containers.isNotEmpty())
			put("playable", containers.sorted().joinToString(","))
	}

/**
 * The query an audio request carries: the id, what to convert to, and what not
 * to convert at all.
 *
 * Hoisted out of [StreamUrls.build] for the reason [videoStreamParams] is -
 * `build` needs Hilt and this does not, and the assertion worth having is that
 * an empty [playable] yields no `playable` parameter whatsoever. That is the
 * audio twin of `the cast route declares nothing`: a receiver handed a URL that
 * declares gets the original while its `LOAD` announced the transcode's type,
 * and refuses the media outright.
 *
 * `format` and `maxBitRate` are omitted together for the original, because the
 * server serves the file directly with no ffmpeg involved at all - unless
 * [playable] says this device cannot play it, in which case it arrives as FLAC.
 * They are what the declaration is *not*: these say what to produce, [playable]
 * says what this device can play as stored.
 *
 * Sorted for the same reason the video list is: a `Set`'s iteration order is not
 * a promise, and this string lands in OkHttp's cache key and the server's log.
 */
internal fun audioStreamParams(
	id: String,
	quality: AudioQuality,
	playable: Set<String>,
): Map<String, String> =
	buildMap {
		put("id", id)
		if (quality.format != AudioFormat.ORIGINAL) {
			put("format", quality.format.param)
			put("maxBitRate", quality.bitRate.toString())
		}
		if (playable.isNotEmpty())
			put("playable", playable.sorted().joinToString(","))
	}

/**
 * The single builder of stream URLs, used by both the download queue and the
 * player.
 *
 * Deliberately not reached through `LibraryRepository`: that class writes the
 * mirror `PinRepository` reads, and a dependency back would close the loop -
 * the same reasoning that kept `PinRepository` from using `CoverUrls`.
 */
@Singleton
class StreamUrls @Inject constructor(
	private val registry: ServerRegistry,
	private val clients: SubsonicClientFactory,
	private val settings: SettingsStore,
	private val audioCache: AudioCache,
) {

	/**
	 * Always the current setting: pinning is the user asking for this track at
	 * the quality they have chosen, so an older copy does not satisfy it.
	 */
	suspend fun forDownload(
		ref: ItemRef,
		audioOnlyVideo: Boolean = false,
		playable: Set<String> = emptySet(),
	): StreamTarget? =
		build(ref, settings.audioQuality.first(), audioOnlyVideo, playable)

	/**
	 * Prefers a quality already held in full, so a library downloaded at an
	 * earlier setting keeps playing from disk instead of being re-fetched.
	 *
	 * The URL is built for the held quality rather than the current one: if the
	 * copy turns out to need topping up, the bytes that arrive have to match the
	 * bytes already there.
	 *
	 * [playable] is what the request declares this device plays as stored,
	 * and the default declares nothing. **This function serves the Cast route as
	 * well as the local player** - `CastUrls.directTarget` reaches the server
	 * through it in three of its branches - so the declaration cannot live in
	 * here, only at the call site that will read the bytes itself.
	 */
	suspend fun forPlayback(
		ref: ItemRef,
		audioOnlyVideo: Boolean = false,
		playable: Set<String> = emptySet(),
	): StreamTarget? {
		val preferred = settings.audioQuality.first().let {
			if (audioOnlyVideo) it.forVideoAudio() else it
		}
		// Off the main thread: this walks the cache index, and onAddMediaItems
		// runs on the player's thread.
		val held = withContext(Dispatchers.IO) {
			audioCache.heldTagOf(ref.encode(), preferred)
		}?.let(AudioQuality::parse)
		return build(ref, held ?: preferred, audioOnlyVideo, playable)
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
	 * The returned `cacheKey` names bytes nobody will store: the receiver
	 * fetches this URL, not the phone. It is carried only because
	 * [StreamTarget] travels as one value, and the direct cast path returns
	 * without ever reading it.
	 */
	suspend fun forCastOriginal(ref: ItemRef): StreamTarget? =
		build(ref, AudioQuality.ORIGINAL)

	/**
	 * The original converted to FLAC, for a receiver that cannot play the
	 * stored file. Lossless rather than a lossy encode, so a lossy source is
	 * given no second generation of loss: the rule the server applies to an
	 * original this device cannot play, and to a film's soundtrack on its own
	 * cast (`CAST_AUDIO_ONLY_FORMAT`). The cache key is carried for the reason
	 * [forCastOriginal] gives; the quality says FLAC so the track info dialog
	 * can tell this from the original sent untouched.
	 */
	suspend fun forCastLossless(ref: ItemRef): StreamTarget? =
		withContext(Dispatchers.IO) {
			val config = registry.get(ref.server) ?: return@withContext null
			StreamTarget(
				url = clients.clientFor(config).url(
					"stream",
					mapOf("id" to ref.id, "format" to "flac"),
				),
				quality = LOSSLESS,
				cacheKey = CacheKeys.of(ref, LOSSLESS),
				mimeType = MimeTypes.AUDIO_FLAC,
			)
		}

	/**
	 * A video's soundtrack copied out as it stands, in the [format] the browse
	 * response named for it, for a receiver that will be told [mime] up front.
	 *
	 * No `maxBitRate`: the server only copies when none is given, since a
	 * video's stored bitrate is the container's and says nothing about its
	 * audio.
	 */
	suspend fun forSoundtrackCopy(ref: ItemRef, format: String, mime: String): StreamTarget? =
		withContext(Dispatchers.IO) {
			val config = registry.get(ref.server) ?: return@withContext null
			StreamTarget(
				url = clients.clientFor(config).url(
					"stream",
					mapOf("id" to ref.id, "format" to format),
				),
				quality = AudioQuality.ORIGINAL,
				cacheKey = CacheKeys.of(ref, AudioQuality.ORIGINAL),
				mimeType = mime,
			)
		}

	/**
	 * Where to fetch a video from, and how it will have to be seeked.
	 *
	 * Two parameters are conspicuously absent, and both omissions are
	 * load-bearing:
	 *
	 *  - **`format`** is validated against the *audio* target table, so a video
	 *    container name is rejected outright and an audio one asks the server
	 *    for the soundtrack alone. That second behaviour is now a feature -
	 *    `SettingsStore.videoAudioOnly` - but it is reached by resolving the
	 *    item through the *audio* path instead, never from here. This builder
	 *    is for someone who wants the picture, and a format here would take it
	 *    away. `web/app.js` splits the same two cases the same way.
	 *  - **`maxBitRate`** sets `constrained` server-side, which forces the
	 *    re-encode tier and demotes a file that could have been served straight
	 *    off disk.
	 *
	 * [progressive] is the caller's answer to whether the server will hand
	 * over the file as stored. Only then does `stream.view` carry a
	 * `Content-Length` and answer `Range`; anything it has to remux or
	 * re-encode arrives as a pipe with neither, so it is played as HLS, where
	 * seeking is picking a segment. The answer depends on who reads the bytes,
	 * which is why it is decided at the call site and not here.
	 *
	 * [playable] is a third parameter that is *usually* absent, and the default
	 * is the safety. It tells the server we demux those containers ourselves -
	 * the video half of one parameter that also carries the audio declaration,
	 * see [audioStreamParams] - so it can skip a remux it would otherwise pay.
	 * But the same URL builder serves the Cast route, and a receiver demuxes
	 * none of them: handed Matroska it refuses the media outright, which reads
	 * as a broken file rather than a misrouted one.
	 *
	 * So it is passed at the call site that plays locally and nowhere else,
	 * exactly as `CastUrls.paced()` is applied at the route rather than here.
	 * Not sent on the HLS branch: the playlist is fragmented MP4 whatever the
	 * container was, so declaring there would be a claim the server cannot
	 * act on.
	 */
	suspend fun forVideo(
		ref: ItemRef,
		progressive: Boolean,
		playable: Set<String> = emptySet(),
	): VideoTarget? =
		withContext(Dispatchers.IO) {
			val config = registry.get(ref.server) ?: return@withContext null
			val client = clients.clientFor(config)

			if (progressive) {
				VideoTarget(
					url = client.url(
						"stream",
						videoStreamParams(ref.id, playable),
					),
					// No declared type: sniffing is the only honest answer, the
					// same argument AudioFormat.ORIGINAL makes. A VP9/Opus .mkv
					// is served relabelled video/webm, so the entry's own
					// contentType is wrong in exactly the cases that matter.
					mimeType = null,
					isHls = false,
				)
			} else {
				VideoTarget(
					// The playlist copies this request's auth parameters onto
					// every segment URL, so the player needs no context from
					// here to fetch them.
					url = client.url("hls.m3u8", mapOf("id" to ref.id), suffix = ""),
					mimeType = MimeTypes.APPLICATION_M3U8,
					isHls = true,
				)
			}
		}

	/**
	 * [audioOnlyVideo] says [ref] names a video and only its soundtrack is
	 * wanted. It changes exactly one thing - [AudioQuality.ORIGINAL] cannot be
	 * expressed for a video, since it is spelled by sending no `format` and
	 * that fetches the film - but it has to be threaded all the way here
	 * because the cache key is derived from the same quality value.
	 */
	private suspend fun build(
		ref: ItemRef,
		wanted: AudioQuality,
		audioOnlyVideo: Boolean = false,
		playable: Set<String> = emptySet(),
	): StreamTarget? =
		withContext(Dispatchers.IO) {
			val config = registry.get(ref.server) ?: return@withContext null
			val quality = if (audioOnlyVideo) wanted.forVideoAudio() else wanted

			// Nothing for a video played as audio, and not merely because it
			// would be ignored: `plan_transcode` guards the audio declaration on
			// `!song.is_video`, and a parameter the server drops on the floor
			// invites the next reader to believe it does something.
			val declared: Set<String> = if (audioOnlyVideo) emptySet() else playable

			StreamTarget(
				url = clients.clientFor(config).url(
					"stream",
					audioStreamParams(ref.id, quality, declared),
				),
				quality = quality,
				cacheKey = CacheKeys.of(ref, quality),
				// Null the moment anything was declared: the response may be the
				// file as it stands rather than the format asked for, and this
				// value reaches ExoPlayer, the download index and - where it
				// cannot be sniffed away - a Cast receiver. Sniffing is then the
				// only honest answer, the same argument AudioFormat.ORIGINAL
				// already makes for itself.
				mimeType = if (declared.isEmpty()) quality.format.mime else null,
			)
		}

	private companion object {
		val LOSSLESS = AudioQuality(AudioFormat.FLAC, 0)
	}
}
