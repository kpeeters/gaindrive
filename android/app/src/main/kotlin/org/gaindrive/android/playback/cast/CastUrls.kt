package org.gaindrive.android.playback.cast

import android.util.Log
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.flow.first
import kotlinx.coroutines.withContext
import okhttp3.OkHttpClient
import okhttp3.Request
import org.gaindrive.android.data.ServerRegistry
import org.gaindrive.android.data.SettingsStore
import org.gaindrive.android.data.StreamTarget
import org.gaindrive.android.data.StreamUrls
import org.gaindrive.android.data.cache.AudioCache
import org.gaindrive.android.data.model.AudioQuality
import org.gaindrive.android.data.model.ItemRef
import org.gaindrive.android.playback.CastSource
import java.util.concurrent.TimeUnit
import javax.inject.Inject
import javax.inject.Singleton

/**
 * Who serves the bytes to the receiver. Three values rather than a `bridged`
 * flag, because the third is not a relay at all and is the one worth telling
 * apart: it is the branch that works with no connectivity, so a user asking
 * "why is this playing when the server is unreachable" has an answer.
 *
 * Note none of these is "the server drives the Chromecast" — that mode is the
 * web client's and is deliberately not ported; see `CAST.md`. The phone always
 * holds the control channel.
 */
enum class CastRoute {
	/** The receiver fetches `stream.view` from the owning server itself. */
	DIRECT,

	/** The receiver fetches from [CastBridge], which fetches from the server. */
	RELAY,

	/** The receiver fetches from [CastBridge], which serves a downloaded copy. */
	LOCAL,
}

/**
 * What to hand a Cast receiver for a track: a URL it can actually fetch, the
 * MIME type to declare for it, and how it got to be that URL.
 *
 * [url] points at the owning server, or at the bridge relaying that server, or
 * at the bridge serving a copy already on the device — and the receiver cannot
 * tell the three apart. The decision is made per track, so a queue that spans
 * servers may mix them, which is the one place several servers make casting
 * easier rather than harder. [route] is the same decision written down, for the
 * one observer that does need to tell them apart: the track info dialog.
 */
data class CastTarget(
	val url: String,
	val mimeType: String?,
	val route: CastRoute,
	/** What the server was asked to send. Null for video, which is never asked. */
	val quality: AudioQuality? = null,
) {
	/** Whether artwork has to travel the same road; see [CastUrls.artworkFor]. */
	val bridged: Boolean get() = route != CastRoute.DIRECT
}

/**
 * Whether a Cast receiver will decode a file of this type as it stands.
 *
 * The set is what the Default Media Receiver documents, intersected with the
 * types `src/codecs.hh` can report. It reports seven for audio and only
 * `audio/x-ms-wma` is missing here — but this is an allowlist rather than a
 * one-entry denylist on purpose: a codec added to the server later would
 * otherwise be sent to a receiver that cannot play it, and demoting to a
 * transcode is the safe direction of a wrong guess.
 *
 * **ALAC is the gap it cannot close.** `songs.codec` is a file extension, so
 * the server reports `audio/mp4` for AAC and for ALAC alike and nothing here
 * can tell them apart. An ALAC `.m4a` cast at original quality will still fail.
 *
 * A null type is a queue the system restored from bare media ids, and is
 * refused for the same reason: not knowing is not the same as knowing it is
 * fine.
 *
 * Top-level rather than a member so it can be exercised without `android.util`
 * being loaded — the class it sits beside logs, this function does not.
 */
internal fun castPlaysNatively(mime: String?): Boolean = mime in CAST_NATIVE_TYPES

private val CAST_NATIVE_TYPES = setOf(
	"audio/flac",
	"audio/mpeg",
	"audio/mp4",
	"audio/aac",
	"audio/ogg",
	"audio/wav",
)

@Singleton
class CastUrls @Inject constructor(
	private val streamUrls: StreamUrls,
	private val registry: ServerRegistry,
	private val reachability: CastReachability,
	private val bridge: CastBridge,
	private val audioCache: AudioCache,
	private val httpClient: OkHttpClient,
	private val settings: SettingsStore,
) {

	/**
	 * Decides who fetches the track, and then at what quality.
	 *
	 * That order is the reverse of what this used to do, and it is the whole
	 * shape of `SettingsStore.castOriginal`: the original file is worth sending
	 * only to a receiver that pulls it off the server itself, so the route has
	 * to be settled first. Asking costs nothing — [CastReachability] caches its
	 * answer per server and network, not per track.
	 *
	 * Every other route resolves exactly as local playback would, same quality
	 * and same per-server bitrate cap.
	 *
	 * [source] is what the queue entry already knows. Its MIME types are used
	 * rather than re-derived so that the type declared in the `LOAD` and the type
	 * the bridge puts on the response are the same answer and cannot drift.
	 */
	suspend fun forCast(ref: ItemRef, source: CastSource): CastTarget? {
		if (source.isVideo) return forVideo(ref, source)

		val config = registry.get(ref.server) ?: return null

		if (reachability.canReachDirectly(config)) {
			val direct = directTarget(ref, source) ?: return null
			return CastTarget(
				direct.url,
				direct.mimeType ?: source.sourceMime,
				CastRoute.DIRECT,
				direct.quality,
			)
		}

		val target = streamUrls.forPlayback(ref) ?: return null
		val mime = target.mimeType ?: source.sourceMime

		// Downloaded: serve the copy on the device rather than fetching it back
		// from the server through the phone. Not an optimisation — it is the
		// only branch that works with no connectivity at all, which is the state
		// a downloaded library exists for, and without it casting a downloaded
		// track offline handed the receiver a server URL nothing could fetch.
		//
		// Below the direct branch on purpose: a receiver that can reach the
		// server should still fetch for itself, because that survives the phone
		// sleeping, going out of range or running flat.
		storedCopy(target.cacheKey)?.let { length ->
			bridge.publishLocal(target.cacheKey, mime, length)?.let { url ->
				return CastTarget(url, mime, CastRoute.LOCAL, target.quality)
			}
		}

		// A bridge that will not start is not a reason to play nothing: the
		// direct URL may still work, since the probe is a guess about the
		// receiver, not a measurement of it. That fallback really does hand
		// over a server URL, so it reports DIRECT — the route says what was
		// done, not what was intended.
		val relayed = bridge.publish(target.url)
		return CastTarget(
			relayed ?: target.url,
			mime,
			if (relayed != null) CastRoute.RELAY else CastRoute.DIRECT,
			target.quality,
		)
	}

	/**
	 * What to hand a receiver that will fetch from the server itself.
	 *
	 * Two conditions have to hold before the original is sent, and the second
	 * is not a caution but a fix: a `.wma` handed over as it stands fails the
	 * `LOAD` with `IDLE`/`ERROR`, is retried once by [LoadRetryWatcher], fails
	 * again and stalls — with nothing on screen to say why. Falling back to the
	 * streaming quality plays the track instead, and the log line is what makes
	 * "why is this one still Opus" answerable.
	 */
	private suspend fun directTarget(ref: ItemRef, source: CastSource): StreamTarget? {
		if (!settings.castOriginal.first()) return streamUrls.forPlayback(ref)
		if (!castPlaysNatively(source.sourceMime)) {
			Log.i(
				TAG,
				"original ${source.sourceMime ?: "type unknown"} is not one the" +
					" receiver takes; sending the streaming quality",
			)
			return streamUrls.forPlayback(ref)
		}
		return streamUrls.forCastOriginal(ref)
	}

	/**
	 * The same decision for a video, which differs from audio in three ways.
	 *
	 * Only one tier is offered: the one the server can hand over as a real MP4
	 * with a `Content-Length` and byte ranges, which is what `nativeSeek` names.
	 * A file the server can only re-encode has no seekable form at all — its
	 * answer is chunked with `Accept-Ranges: none` — and the fix for that is
	 * `hls.m3u8`, whose relative segment URIs the bridge cannot resolve. So it
	 * is refused here, and the UI refuses it earlier and in words.
	 *
	 * Nothing about quality or bitrate is sent, for the reason set out on
	 * [StreamUrls.forVideo]: any `format` or `maxBitRate` sets `constrained`
	 * server-side and demotes a file that could have been served off disk
	 * untouched.
	 *
	 * And there is no stored-copy branch, because video never enters the byte
	 * cache — `GainDriveMediaSourceFactory` hands it the bare network factory —
	 * so looking would only ever miss.
	 */
	private suspend fun forVideo(ref: ItemRef, source: CastSource): CastTarget? {
		if (!source.nativeSeek) return null
		val target = streamUrls.forVideo(ref, nativeSeek = true) ?: return null
		val config = registry.get(ref.server) ?: return null

		// The server's own answer, not one worked out from the codecs: it is
		// present exactly when the file will be remuxed, and the source type is
		// wrong in precisely that case.
		val mime = source.transcodedMime ?: source.sourceMime

		warmTranscode(target.url)

		if (reachability.canReachDirectly(config)) {
			return CastTarget(target.url, mime, CastRoute.DIRECT)
		}
		val relayed = bridge.publish(target.url)
		return CastTarget(
			relayed ?: target.url,
			mime,
			if (relayed != null) CastRoute.RELAY else CastRoute.DIRECT,
		)
	}

	/**
	 * Fetches one byte, so that a remux happens while the phone waits rather
	 * than while the receiver does.
	 *
	 * An H.264/AAC `.mkv` is not directly playable — the container is not one a
	 * browser takes — so the server remuxes it, and its transcode cache is
	 * *blocking*: nothing is sent until ffmpeg has written the whole file. For a
	 * multi-gigabyte film that is minutes, and a receiver that has gone that long
	 * without data gives up with a network error. Untreated, the first cast of
	 * every `.mkv` fails and the second works, which reads as a random fault.
	 *
	 * A one-byte range is enough to force the build, and costs nothing on a file
	 * the server serves directly. Failure is ignored on purpose: this is a warm,
	 * not a fetch, and if it went wrong the receiver's own request is still
	 * entitled to try.
	 */
	private suspend fun warmTranscode(url: String) {
		withContext(Dispatchers.IO) {
			runCatching {
				val request = Request.Builder().url(url).header("Range", "bytes=0-0").build()
				warmClient.newCall(request).execute().use { response ->
					Log.i(
						TAG,
						"warm ${response.code}" +
							" transcode=${response.header("X-Gaindrive-Transcode") ?: "none"}",
					)
				}
			}.onFailure { Log.w(TAG, "warm failed: $it") }
		}
	}

	/**
	 * The shared client with a read timeout a remux can finish inside. Its 30 s
	 * is sized for audio, and derived rather than changed because Retrofit and
	 * Coil are on the same client and want the short one.
	 */
	private val warmClient by lazy {
		httpClient.newBuilder().readTimeout(WARM_TIMEOUT_MINUTES, TimeUnit.MINUTES).build()
	}

	/**
	 * The length of a complete copy held under [cacheKey], or null if there is
	 * not one that can be served.
	 *
	 * Both halves are required. A partial copy would play until the bytes ran
	 * out, and a copy of unknown length cannot be given a `Content-Length` or a
	 * ranged answer — which is most of what a receiver asks for.
	 */
	private suspend fun storedCopy(cacheKey: String): Long? =
		// Off the main thread: this walks the cache index, the same reason
		// StreamUrls.forPlayback does it.
		withContext(Dispatchers.IO) {
			audioCache.storedLength(cacheKey)?.takeIf { audioCache.isFullyCached(cacheKey) }
		}

	/**
	 * Artwork has to travel the same road as the audio. A receiver that cannot
	 * reach the server for one cannot reach it for the other, and a cast session
	 * showing a blank sleeve on the television looks broken.
	 */
	fun artworkFor(url: String?, bridged: Boolean): String? {
		if (url == null) return null
		return if (bridged) bridge.publish(url) ?: url else url
	}

	private companion object {
		const val TAG = "GainDriveCast"

		/**
		 * Generous, because the thing being waited for is a `-c copy` remux of a
		 * whole film and the cost of being wrong is asymmetric: too long only
		 * delays a cast the user is already watching a spinner for, while too
		 * short hands the receiver a stream that has not been built yet.
		 */
		const val WARM_TIMEOUT_MINUTES = 10L
	}
}
