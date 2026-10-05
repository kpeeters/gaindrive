package org.gaindrive.android.data.model

/**
 * A container the server can produce and ExoPlayer can decode.
 *
 * [param] is the `format` value sent to `stream.view`. [mime] is what the media
 * item declares; null for [ORIGINAL], where the container is whatever the file
 * happens to be and extractor sniffing is the only honest answer.
 *
 * [sampleMime] is the *codec* inside that container, which is a different
 * string for Opus and the same one for MP3 - Ogg is a container and MPEG audio
 * is not. It exists so the track info dialog can compare what the decoder
 * reports against what was asked for: since a request declares the formats
 * media3 takes as they stand, the two no longer have to agree, and a mismatch
 * is precisely how "this arrived unconverted" is recognised.
 */
enum class AudioFormat(val param: String, val mime: String?, val sampleMime: String?) {
	ORIGINAL("raw", null, null),
	OPUS("opus", "audio/ogg", "audio/opus"),
	M4A("m4a", "audio/mp4", "audio/mp4a-latm"),
	MP3("mp3", "audio/mpeg", "audio/mpeg"),

	/**
	 * Never chosen: what an original a Cast receiver cannot play is sent as,
	 * so the track info dialog can say so. See `StreamUrls.forCastLossless`.
	 */
	FLAC("flac", "audio/flac", "audio/flac"),
}

/**
 * What quality to ask the server for, and - via [tag] - how to tell bytes of one
 * quality apart from another in the cache.
 *
 * [ORIGINAL], or a rough ceiling of [bitRate] with [format] as what to encode to
 * when the ceiling (or this device) makes an encode unavoidable. A file media3
 * plays that fits under the ceiling is sent as stored, whatever its format -
 * see `MEDIA3_AUDIO`.
 *
 * One value governs downloads, cache-on-play and streaming alike. There is
 * deliberately no per-connection variant: everything played on the phone is
 * cached, so a separate "streaming quality" would only mean storing something
 * different from what a download of the same track would have produced.
 */
data class AudioQuality(val format: AudioFormat, val bitRate: Int) {

	/**
	 * Short, stable and filename-safe. Forms the suffix of a cache key, so
	 * changing how this is spelled orphans every stored track - which is
	 * survivable (LRU reclaims them) but not free.
	 */
	val tag: String
		get() = if (format == AudioFormat.ORIGINAL) "orig" else "${format.param}$bitRate"

	/** What a conversion at this quality produces; [ORIGINAL] has no bitrate. */
	val label: String
		get() = when (format) {
			AudioFormat.ORIGINAL -> "Original"
			AudioFormat.OPUS -> "Opus $bitRate"
			AudioFormat.M4A -> "AAC $bitRate"
			AudioFormat.MP3 -> "MP3 $bitRate"
			AudioFormat.FLAC -> "FLAC"
		}

	/** The setting as a person would say it: a ceiling, not a format. */
	val limitLabel: String
		get() = if (format == AudioFormat.ORIGINAL) "Original" else "up to $bitRate kbps"

	/**
	 * This quality as it applies to a video played for its soundtrack alone.
	 *
	 * [AudioFormat.ORIGINAL] cannot mean anything here. It is expressed by
	 * sending no `format` at all, and for a video that is a request for the
	 * film - the opposite of what was asked for. There is no "the original
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

		/** What a ceiling may convert to, offered in settings. */
		val TRANSCODE_FORMATS = listOf(AudioFormat.OPUS, AudioFormat.M4A, AudioFormat.MP3)

		/** Inverse of [tag]; null when the string is not one we wrote. */
		fun parse(tag: String): AudioQuality? {
			if (tag == "orig") return ORIGINAL
			val format = TRANSCODE_FORMATS.firstOrNull { tag.startsWith(it.param) }
				?: return null
			val rate = tag.removePrefix(format.param).toIntOrNull() ?: return null
			if (rate <= 0) return null
			return AudioQuality(format, rate)
		}
	}
}
