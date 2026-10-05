package org.gaindrive.android.playback

import okhttp3.HttpUrl
import okhttp3.HttpUrl.Companion.toHttpUrlOrNull
import okhttp3.Interceptor
import org.gaindrive.android.data.model.AudioFormat
import org.gaindrive.android.data.model.AudioQuality
import java.util.concurrent.ConcurrentHashMap
import javax.inject.Inject
import javax.inject.Singleton

/**
 * The server's `X-Gaindrive-Sent` for the latest stream of each track: its own
 * account of what it decided to send.
 *
 * The decoder answers most of that for local playback, but not two cases: a
 * Cast receiver decodes nothing on this phone, and an MP3 over the ceiling
 * re-encoded as MP3 has the same codec as the file. Recorded on the media
 * client, so ExoPlayer's reads, the pre-warm, downloads and the cast probe all
 * feed it without knowing.
 *
 * Kept per track with the shape of the request that produced it, because the
 * answer depends on that shape: the same song at another quality, or asked
 * without a declaration as the cast route asks, can be sent differently. A
 * lookup whose shape does not match finds nothing rather than a wrong answer.
 * In memory only: bytes read back from the cache carry no header, and the
 * dialog then falls back to the decoder.
 */
@Singleton
class SentFormats @Inject constructor() {

	private data class Shape(val format: String?, val maxBitRate: Int, val declared: Boolean)

	private data class Entry(val shape: Shape, val sent: String)

	private val latest = ConcurrentHashMap<String, Entry>()

	/** Records a stream response's header; every response passes through untouched. */
	val interceptor = Interceptor { chain ->
		val request = chain.request()
		val response = chain.proceed(request)
		response.header(HEADER)?.let { record(request.url, it) }
		response
	}

	/** Separate from [interceptor] so it can be exercised without a network. */
	internal fun record(url: HttpUrl, sent: String) {
		val id = url.queryParameter("id") ?: return
		if (url.pathSegments.lastOrNull() !in STREAM_ENDPOINTS) return
		latest[key(url, id)] = Entry(shapeOf(url), sent)
	}

	/**
	 * What the server said it sent for this track at [quality], asked with or
	 * without a declaration; null when nothing matching was recorded.
	 */
	fun lookup(serverUrl: String, songId: String, quality: AudioQuality, declared: Boolean): String? {
		val base = serverUrl.toHttpUrlOrNull() ?: return null
		val entry = latest[key(base, songId)] ?: return null
		val shape = when (quality.format) {
			AudioFormat.ORIGINAL -> Shape(null, 0, declared)
			AudioFormat.FLAC -> Shape(AudioFormat.FLAC.param, 0, declared)
			else -> Shape(quality.format.param, quality.bitRate, declared)
		}
		return entry.sent.takeIf { entry.shape == shape }
	}

	private fun key(url: HttpUrl, id: String) = "${url.host}:${url.port}/$id"

	private fun shapeOf(url: HttpUrl) = Shape(
		format = url.queryParameter("format")?.takeIf { it.isNotEmpty() && it != "raw" },
		maxBitRate = url.queryParameter("maxBitRate")?.toIntOrNull() ?: 0,
		declared = !url.queryParameter("playable").isNullOrEmpty(),
	)

	companion object {
		const val HEADER = "X-Gaindrive-Sent"
		private val STREAM_ENDPOINTS = setOf("stream.view", "stream")

		/** The header's value in words, for the track info dialog. */
		fun describe(sent: String, storedSuffix: String?): String {
			val words = sent.split(' ')
			val name = when (val target = words.first()) {
				"original" ->
					return listOfNotNull(storedSuffix?.uppercase(), "unconverted").joinToString(", ")
				"m4a", "aac" -> "AAC"
				"opus" -> "Opus"
				"mp3" -> "MP3"
				"flac" -> "FLAC"
				"ogg", "oga", "vorbis" -> "Vorbis"
				else -> target.uppercase()
			}
			return when {
				words.size > 1 -> "$name ${words[1]} kbps"
				name == "FLAC" -> "FLAC, converted losslessly"
				else -> "$name, converted"
			}
		}
	}
}
