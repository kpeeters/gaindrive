package org.gaindrive.android.data.model

import org.junit.Assert.assertEquals
import org.junit.Assert.assertNotEquals
import org.junit.Test

/**
 * The substitution a video's soundtrack needs, and the reason it is a function
 * rather than a line at each call site: the stream URL and the cache key are
 * derived from the same value, and a URL naming one quality paired with a key
 * naming another stores bytes that will later be served to a request expecting
 * something else.
 */
class AudioQualityTest {

	@Test
	fun `original is unusable for a video and becomes the default`() {
		// ORIGINAL is spelled by sending no `format` at all, which for a video
		// fetches the film - the opposite of asking for its soundtrack.
		assertEquals(AudioQuality.DEFAULT, AudioQuality.ORIGINAL.forVideoAudio())
		assertNotEquals(AudioFormat.ORIGINAL, AudioQuality.ORIGINAL.forVideoAudio().format)
	}

	@Test
	fun `every other quality passes through untouched`() {
		val chosen = AudioQuality(AudioFormat.OPUS, 96)
		assertEquals(chosen, chosen.forVideoAudio())
		assertEquals(chosen, chosen.forVideoAudio().forVideoAudio())
	}

	@Test
	fun `the substituted quality names a real container in its tag`() {
		// The tag is what the cache key carries, so it has to describe the bytes
		// that will arrive rather than what was asked for.
		val tag = AudioQuality.ORIGINAL.forVideoAudio().tag
		assertNotEquals("orig", tag)
		assertEquals(AudioQuality.DEFAULT, AudioQuality.parse(tag))
	}

	@Test
	fun `aac tags round-trip`() {
		val aac = AudioQuality(AudioFormat.M4A, 128)
		assertEquals("m4a128", aac.tag)
		assertEquals(aac, AudioQuality.parse(aac.tag))
	}

	@Test
	fun `the limit reads as a ceiling`() {
		assertEquals("Original", AudioQuality.ORIGINAL.limitLabel)
		assertEquals("up to 160 kbps", AudioQuality.DEFAULT.limitLabel)
	}
}
