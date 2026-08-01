package org.gaindrive.android.data

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
	suspend fun forDownload(ref: ItemRef): StreamTarget? =
		build(ref, settings.audioQuality.first())

	/**
	 * Prefers a quality already held in full, so a library downloaded at an
	 * earlier setting keeps playing from disk instead of being re-fetched.
	 *
	 * The URL is built for the held quality rather than the current one: if the
	 * copy turns out to need topping up, the bytes that arrive have to match the
	 * bytes already there.
	 */
	suspend fun forPlayback(ref: ItemRef): StreamTarget? {
		val preferred = settings.audioQuality.first()
		// Off the main thread: this walks the cache index, and onAddMediaItems
		// runs on the player's thread.
		val held = withContext(Dispatchers.IO) {
			audioCache.heldTagOf(ref.encode(), preferred)
		}?.let(AudioQuality::parse)
		return build(ref, held ?: preferred)
	}

	private suspend fun build(ref: ItemRef, wanted: AudioQuality): StreamTarget? =
		withContext(Dispatchers.IO) {
			val config = registry.get(ref.server) ?: return@withContext null
			// The account ceiling is applied here, where this track's own server
			// is in hand. A queue may span servers, so there is no single
			// "current" cap to read.
			val quality = wanted.cappedBy(limits.capFor(config))

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
