package org.gaindrive.android.playback

import okhttp3.HttpUrl.Companion.toHttpUrl
import org.gaindrive.android.data.model.AudioFormat
import org.gaindrive.android.data.model.AudioQuality
import org.junit.Assert.assertEquals
import org.junit.Assert.assertNull
import org.junit.Test

/**
 * The server's `X-Gaindrive-Sent`, kept per track and handed back only to a
 * question asked in the shape of the request that produced it - the same
 * track at another quality, or without a declaration, can be sent otherwise.
 */
class SentFormatsTest {

	private val server = "http://music.local:4040"
	private val m4a160 = AudioQuality(AudioFormat.M4A, 160)

	private fun recorded(query: String, sent: String) = SentFormats().apply {
		record("$server/rest/stream.view?id=7&$query".toHttpUrl(), sent)
	}

	@Test
	fun `a matching request finds the answer`() {
		val formats = recorded("format=m4a&maxBitRate=160&playable=mpeg%2Fmp3", "original")
		assertEquals("original", formats.lookup(server, "7", m4a160, declared = true))
	}

	/** A cast asks without a declaration, and may well be sent something else. */
	@Test
	fun `a request without the declaration is a different question`() {
		val formats = recorded("format=m4a&maxBitRate=160&playable=mpeg%2Fmp3", "original")
		assertNull(formats.lookup(server, "7", m4a160, declared = false))
	}

	@Test
	fun `another quality is a different question`() {
		val formats = recorded("format=m4a&maxBitRate=160&playable=mpeg%2Fmp3", "original")
		assertNull(formats.lookup(server, "7", AudioQuality(AudioFormat.M4A, 96), declared = true))
	}

	@Test
	fun `the original and a cast in flac are looked up by their own shapes`() {
		val original = recorded("playable=flac%2Fflac", "flac")
		assertEquals("flac", original.lookup(server, "7", AudioQuality.ORIGINAL, declared = true))
		val cast = recorded("format=flac", "flac")
		assertEquals(
			"flac",
			cast.lookup(server, "7", AudioQuality(AudioFormat.FLAC, 0), declared = false),
		)
	}

	@Test
	fun `the header reads as words`() {
		assertEquals("MP3, unconverted", SentFormats.describe("original", "mp3"))
		assertEquals("AAC 160 kbps", SentFormats.describe("m4a 160", "flac"))
		assertEquals("FLAC, converted losslessly", SentFormats.describe("flac", "ogg"))
		assertEquals("Opus 96 kbps", SentFormats.describe("opus 96", "mp3"))
	}
}
