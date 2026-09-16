package org.gaindrive.android.data.cache

/**
 * How many bytes of a stored track are enough to name its container.
 *
 * Twelve: the longest thing looked at is `ftyp` at offset four, and `WAVE` at
 * offset eight.
 */
internal const val SNIFF_BYTES = 12

/**
 * The container [head] is, as a MIME type, or null when it says nothing this
 * recognises.
 *
 * A separate file from `AudioCache` and free of every Android import, for the
 * reason `videoStreamParams` sits outside `StreamUrls`: this is the half worth
 * testing and it needs no device to test. The caller that reads the bytes needs
 * a `DataSource`; deciding what they mean does not.
 *
 * Only the *container* is answered, and only the container is wanted. A
 * `Content-Type` names a container, and the question "did the server convert
 * this?" does not have to be asked to answer it — an Opus transcode and a
 * passed-through `.ogg` are both `audio/ogg`, an MP3 transcode and a
 * passed-through `.mp3` are both `audio/mpeg`.
 *
 * [read] is how much of [head] was actually filled, which is not always
 * [SNIFF_BYTES]: a stored span can be shorter than the sniff, and reading past
 * it would test whatever the array was initialised with.
 */
internal fun sniffAudioMime(head: ByteArray, read: Int): String? {
	fun at(offset: Int, vararg want: Char): Boolean {
		if (read < offset + want.size) return false
		return want.withIndex().all { (i, c) -> head[offset + i] == c.code.toByte() }
	}

	// Fixed-offset signatures first. None of them can collide with the frame
	// sync below, which is why that one is allowed to be as loose as it is.
	if (at(0, 'O', 'g', 'g', 'S')) return "audio/ogg"
	if (at(0, 'f', 'L', 'a', 'C')) return "audio/flac"
	if (at(4, 'f', 't', 'y', 'p')) return "audio/mp4"
	if (at(0, 'R', 'I', 'F', 'F') && at(8, 'W', 'A', 'V', 'E')) return "audio/wav"
	// An ID3 tag says MPEG audio without the frame header being reachable: the
	// tag sits in front of it and can be any length.
	if (at(0, 'I', 'D', '3')) return "audio/mpeg"

	// A bare frame sync — eleven set bits — is MPEG audio or ADTS AAC, and the
	// two are told apart by the layer field that follows it. ADTS leaves those
	// bits zero, which is not a legal layer for an MPEG frame, so the test is
	// exact rather than a heuristic.
	if (read >= 2) {
		val b0 = head[0].toInt() and 0xFF
		val b1 = head[1].toInt() and 0xFF
		if (b0 == 0xFF && (b1 and 0xE0) == 0xE0)
			return if ((b1 and 0x06) == 0) "audio/aac" else "audio/mpeg"
	}
	return null
}
