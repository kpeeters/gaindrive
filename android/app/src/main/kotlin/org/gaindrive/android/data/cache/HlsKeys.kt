package org.gaindrive.android.data.cache

import android.net.Uri
import androidx.media3.datasource.cache.CacheKeyFactory
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.launch
import org.gaindrive.android.data.ServerRegistry
import org.gaindrive.android.data.model.AudioFormat
import org.gaindrive.android.data.model.AudioQuality
import org.gaindrive.android.data.model.ItemRef
import org.gaindrive.android.data.model.ServerId
import javax.inject.Inject
import javax.inject.Singleton
import kotlin.math.ceil

/**
 * Cache keys for a film's soundtrack played or stored as HLS
 * (`StreamUrls.forSoundtrackHls`).
 *
 * Every playlist and segment URL carries the caller's credentials, salt
 * included, so the URL would be a different key after every restart. These
 * keys name what the bytes are instead:
 * `hls:<serverId>/<songId>@<tag>/<part>`, with `<part>` one of `playlist`,
 * `init` or `seg/<k>`, and `<tag>` the [AudioQuality.tag] of the rendition.
 *
 * The `hls:` prefix is load-bearing. [CacheKeys.refKeyOf] cuts at the first
 * '@', and without the prefix one stored segment would read as the whole film
 * being here to everything that asks whether a track is stored.
 */
@Singleton
class HlsKeys @Inject constructor(registry: ServerRegistry, scope: CoroutineScope) {

	/**
	 * Each server's `<base>/rest/`, longest first so a server living under
	 * another's path still wins. A snapshot rather than a lookup, because
	 * [factory] runs on loader threads that can neither suspend nor wait.
	 */
	@Volatile
	private var bases: List<Pair<String, ServerId>> = emptyList()

	init {
		scope.launch {
			registry.servers.collect { list ->
				bases = list.map { "${it.url}/rest/" to it.id }
					.sortedByDescending { it.first.length }
			}
		}
	}

	/**
	 * For every cache these URLs pass through. A key set on the request wins,
	 * which is the default rule and what songs rely on; then a soundtrack
	 * URL's own key; then the URL, which is the default's fallback.
	 */
	val factory = CacheKeyFactory { spec ->
		spec.key ?: keyFor(spec.uri) ?: spec.uri.toString()
	}

	/** Null for anything that is not a soundtrack playlist or segment. */
	fun keyFor(uri: Uri): String? {
		val url = uri.toString()
		val server = bases.firstOrNull { url.startsWith(it.first) }?.second ?: return null
		val id = uri.getQueryParameter("id") ?: return null
		val quality = renditionOf(
			uri.getQueryParameter("audioOnly"),
			uri.getQueryParameter("audioBitRate"),
		) ?: return null
		val part = when (uri.lastPathSegment) {
			"hls.m3u8", "hls.view" -> PLAYLIST
			"hlsSegment.view" ->
				if (uri.getQueryParameter("init") == "true") INIT
				else SEG + (uri.getQueryParameter("seg")?.toIntOrNull() ?: return null)
			else -> return null
		}
		return keyOf(ItemRef(server, id), quality, part)
	}

	companion object {
		private const val PREFIX = "hls:"
		private const val PLAYLIST = "playlist"
		private const val INIT = "init"
		private const val SEG = "seg/"

		/** Mirrors `SEGMENT_SECS` and `MIN_TAIL` in `src/hls.cc`. */
		private const val SEGMENT_MS = 6_000L
		private const val MIN_TAIL_SECS = 2.0

		fun isHls(key: String): Boolean = key.startsWith(PREFIX)

		/**
		 * The rendition a soundtrack URL names. Only the two codecs the server
		 * encodes soundtrack segments to; anything else is not one of ours.
		 */
		fun renditionOf(codec: String?, kbps: String?): AudioQuality? {
			val format = when (codec) {
				"opus" -> AudioFormat.OPUS
				"aac" -> AudioFormat.M4A
				else -> return null
			}
			return AudioQuality(format, kbps?.toIntOrNull() ?: return null)
		}

		fun keyOf(film: ItemRef, quality: AudioQuality, part: String): String =
			"$PREFIX${CacheKeys.of(film, quality)}/$part"

		/** The film and rendition [key] belongs to; null for any other key. */
		fun filmOf(key: String): Pair<ItemRef, AudioQuality>? {
			if (!isHls(key)) return null
			val rest = key.removePrefix(PREFIX)
			val at = rest.indexOf('@')
			val slash = if (at < 0) -1 else rest.indexOf('/', at)
			if (slash < 0) return null
			val film = ItemRef.decode(rest.substring(0, at)) ?: return null
			val quality = AudioQuality.parse(rest.substring(at + 1, slash)) ?: return null
			return film to quality
		}

		/**
		 * How many segments the server cuts a soundtrack of [durationSecs]
		 * into: six-second steps from zero, with a last piece shorter than two
		 * seconds folded into the one before, as `Hls::plan()` does.
		 *
		 * Only for protecting segments before their playlist has been seen.
		 * The downloader reads the real cuts from the playlist. Off by one at
		 * the very end at worst, when the server's fractional duration and the
		 * whole seconds the browse response carries fall either side of the
		 * two-second rule.
		 */
		fun segmentCount(durationSecs: Double): Int {
			if (durationSecs <= 0) return 0
			var n = ceil(durationSecs * 1000 / SEGMENT_MS).toInt()
			if (n > 1 && durationSecs - (n - 1) * SEGMENT_MS / 1000.0 < MIN_TAIL_SECS) n -= 1
			return n
		}

		/**
		 * Every key one range of a film needs to play: the playlist and init
		 * segment it shares with the rest of the film, and the media segments
		 * it overlaps. [endMs] may run past the end; it is clamped.
		 */
		fun keysCovering(
			film: ItemRef,
			quality: AudioQuality,
			durationSecs: Double,
			startMs: Long,
			endMs: Long,
		): List<String> {
			val n = segmentCount(durationSecs)
			if (n == 0 || endMs <= startMs) return emptyList()
			val first = (startMs / SEGMENT_MS).toInt().coerceIn(0, n - 1)
			val last = ((endMs - 1) / SEGMENT_MS).coerceAtMost(n - 1L).toInt()
			return listOf(keyOf(film, quality, PLAYLIST), keyOf(film, quality, INIT)) +
				(first..last).map { keyOf(film, quality, SEG + it) }
		}
	}
}
