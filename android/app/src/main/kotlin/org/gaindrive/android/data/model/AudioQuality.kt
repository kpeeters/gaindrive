package org.gaindrive.android.data.model

/**
 * A container the server can produce and ExoPlayer can decode.
 *
 * [param] is the `format` value sent to `stream.view`. [mime] is what the media
 * item declares; null for [ORIGINAL], where the container is whatever the file
 * happens to be and extractor sniffing is the only honest answer.
 *
 * [sampleMime] is the *codec* inside that container, which is a different
 * string for Opus and the same one for MP3 — Ogg is a container and MPEG audio
 * is not. It exists so the track info dialog can compare what the decoder
 * reports against what was asked for: since a request declares the formats
 * media3 takes as they stand, the two no longer have to agree, and a mismatch
 * is precisely how "this arrived unconverted" is recognised.
 */
enum class AudioFormat(val param: String, val mime: String?, val sampleMime: String?) {
	ORIGINAL("raw", null, null),
	OPUS("opus", "audio/ogg", "audio/opus"),

	/**
	 * Never chosen directly — it is what an account bitrate cap turns a request
	 * for [ORIGINAL] into, because that is what the server sends in that case.
	 * See [AudioQuality.cappedBy].
	 */
	MP3("mp3", "audio/mpeg", "audio/mpeg"),
}

/**
 * What quality to ask the server for, and — via [tag] — how to tell bytes of one
 * quality apart from another in the cache.
 *
 * One value governs downloads, cache-on-play and streaming alike. There is
 * deliberately no per-connection variant: everything played on the phone is
 * cached, so a separate "streaming quality" would only mean storing something
 * different from what a download of the same track would have produced.
 */
data class AudioQuality(val format: AudioFormat, val bitRate: Int) {

	/**
	 * Short, stable and filename-safe. Forms the suffix of a cache key, so
	 * changing how this is spelled orphans every stored track — which is
	 * survivable (LRU reclaims them) but not free.
	 */
	val tag: String
		get() = if (format == AudioFormat.ORIGINAL) "orig" else "${format.param}$bitRate"

	/** For the settings row; [ORIGINAL] has no bitrate to name. */
	val label: String
		get() = when (format) {
			AudioFormat.ORIGINAL -> "Original"
			AudioFormat.OPUS -> "Opus $bitRate"
			AudioFormat.MP3 -> "MP3 $bitRate"
		}

	/**
	 * Applies the account's `maxBitRate` ceiling, which the server enforces
	 * whatever the client asks for.
	 *
	 * The [AudioFormat.ORIGINAL] branch is load-bearing rather than cosmetic: a
	 * capped account asking for the raw file receives *mp3 at the cap* instead
	 * (`src/streamer.cc`, the bitrate-limit branch). Without modelling that, the
	 * cache key would claim `@orig` for bytes that are really MP3, and a later
	 * request for genuinely original audio would be served those bytes.
	 */
	fun cappedBy(accountCap: Int): AudioQuality = when {
		accountCap <= 0 -> this
		format == AudioFormat.ORIGINAL -> AudioQuality(AudioFormat.MP3, accountCap)
		bitRate > accountCap -> copy(bitRate = accountCap)
		else -> this
	}

	/**
	 * This quality as it applies to a video played for its soundtrack alone.
	 *
	 * [AudioFormat.ORIGINAL] cannot mean anything here. It is expressed by
	 * sending no `format` at all, and for a video that is a request for the
	 * film — the opposite of what was asked for. There is no "the original
	 * audio track" the server can be asked for either: extracting a soundtrack
	 * is a re-encode, so a container has to be named. [DEFAULT] is that name.
	 *
	 * Every other quality passes through, so someone who set 96 kbps to save
	 * data gets 96 kbps here too.
	 *
	 * One function rather than the substitution written at each call site,
	 * because the cache key is derived from the same value: a URL naming one
	 * quality paired with a key naming another stores bytes that will later be
	 * served to a request expecting something else.
	 */
	fun forVideoAudio(): AudioQuality =
		if (format == AudioFormat.ORIGINAL) DEFAULT else this

	companion object {
		/**
		 * Opus at 160 kbps: transparent enough for headphones on a phone, and
		 * around a fifth the size of the FLAC it usually replaces.
		 */
		val DEFAULT = AudioQuality(AudioFormat.OPUS, 160)

		val ORIGINAL = AudioQuality(AudioFormat.ORIGINAL, 0)

		/** Offered in settings, low to high. */
		val BITRATES = listOf(96, 128, 160, 192, 256)

		/** Inverse of [tag]; null when the string is not one we wrote. */
		fun parse(tag: String): AudioQuality? {
			if (tag == "orig") return ORIGINAL
			val format = AudioFormat.entries.firstOrNull {
				it != AudioFormat.ORIGINAL && tag.startsWith(it.param)
			} ?: return null
			val rate = tag.removePrefix(format.param).toIntOrNull() ?: return null
			if (rate <= 0) return null
			return AudioQuality(format, rate)
		}
	}
}
