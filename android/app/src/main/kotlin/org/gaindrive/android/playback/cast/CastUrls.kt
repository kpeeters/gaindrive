package org.gaindrive.android.playback.cast

import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.withContext
import org.gaindrive.android.data.ServerRegistry
import org.gaindrive.android.data.StreamUrls
import org.gaindrive.android.data.cache.AudioCache
import org.gaindrive.android.data.model.ItemRef
import javax.inject.Inject
import javax.inject.Singleton

/**
 * What to hand a Cast receiver for a track: a URL it can actually fetch, and the
 * MIME type to declare for it.
 *
 * [url] points at the owning server, or at the bridge relaying that server, or
 * at the bridge serving a copy already on the device — and the receiver cannot
 * tell the three apart. The decision is made per track, so a queue that spans
 * servers may mix them, which is the one place several servers make casting
 * easier rather than harder.
 */
data class CastTarget(
	val url: String,
	val mimeType: String?,
	val bridged: Boolean,
)

@Singleton
class CastUrls @Inject constructor(
	private val streamUrls: StreamUrls,
	private val registry: ServerRegistry,
	private val reachability: CastReachability,
	private val bridge: CastBridge,
	private val audioCache: AudioCache,
) {

	/**
	 * Resolves the stream URL exactly as local playback would — same quality,
	 * same per-server bitrate cap — and then decides who fetches it.
	 *
	 * [sourceMime] is the file's own type from the queue entry, used when the
	 * chosen quality is the original file and the transcode's MIME therefore
	 * does not apply. Resolved here rather than by the caller so that the type
	 * declared in the `LOAD` and the type the bridge puts on the response are
	 * the same answer and cannot drift.
	 */
	suspend fun forCast(ref: ItemRef, sourceMime: String?): CastTarget? {
		val target = streamUrls.forPlayback(ref) ?: return null
		val config = registry.get(ref.server) ?: return null
		val mime = target.mimeType ?: sourceMime

		if (reachability.canReachDirectly(config)) {
			return CastTarget(target.url, mime, bridged = false)
		}

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
				return CastTarget(url, mime, bridged = true)
			}
		}

		// A bridge that will not start is not a reason to play nothing: the
		// direct URL may still work, since the probe is a guess about the
		// receiver, not a measurement of it.
		val bridged = bridge.publish(target.url)
		return CastTarget(bridged ?: target.url, mime, bridged = bridged != null)
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
}
