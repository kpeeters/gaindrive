package org.gaindrive.android.playback.cast

import org.gaindrive.android.data.ServerRegistry
import org.gaindrive.android.data.StreamUrls
import org.gaindrive.android.data.model.ItemRef
import javax.inject.Inject
import javax.inject.Singleton

/**
 * What to hand a Cast receiver for a track: a URL it can actually fetch, and the
 * MIME type to declare for it.
 *
 * [url] points either at the owning server or at the bridge, and the receiver
 * cannot tell the difference. The decision is made per track, so a queue that
 * spans servers may mix the two — which is the one place several servers make
 * casting easier rather than harder.
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
) {

	/**
	 * Resolves the stream URL exactly as local playback would — same quality,
	 * same per-server bitrate cap — and then decides who fetches it.
	 */
	suspend fun forCast(ref: ItemRef): CastTarget? {
		val target = streamUrls.forPlayback(ref) ?: return null
		val config = registry.get(ref.server) ?: return null

		if (reachability.canReachDirectly(config)) {
			return CastTarget(target.url, target.mimeType, bridged = false)
		}

		// A bridge that will not start is not a reason to play nothing: the
		// direct URL may still work, since the probe is a guess about the
		// receiver, not a measurement of it.
		val bridged = bridge.publish(target.url)
		return CastTarget(bridged ?: target.url, target.mimeType, bridged = bridged != null)
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
