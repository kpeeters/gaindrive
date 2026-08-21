package org.gaindrive.android.playback.cast

import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Test

/**
 * The gate on sending a Cast receiver the file as it stands.
 *
 * A regression table rather than a smoke test, because the cost of a wrong
 * `true` is invisible: the `LOAD` fails, is retried once, fails again and the
 * player stalls with nothing on screen. The two tables below are the complete
 * set of seven content types `src/codecs.hh` can report for audio, so adding a
 * codec there without revisiting this list shows up here as a case nobody
 * wrote.
 */
class CastQualityTest {

	@Test
	fun `every type the receiver documents is sent as it stands`() {
		for (mime in listOf(
			"audio/flac",
			"audio/mpeg",
			"audio/mp4",
			"audio/aac",
			"audio/ogg",
			"audio/wav",
		)) {
			assertTrue(mime, castPlaysNatively(mime))
		}
	}

	/** The one of those seven a Chromecast cannot decode. */
	@Test
	fun `wma is transcoded instead`() {
		assertFalse(castPlaysNatively("audio/x-ms-wma"))
	}

	/**
	 * A queue the system restored from bare media ids carries no content type.
	 * Not knowing is not the same as knowing it is fine.
	 */
	@Test
	fun `an unknown type is transcoded instead`() {
		assertFalse(castPlaysNatively(null))
		assertFalse(castPlaysNatively(""))
		assertFalse(castPlaysNatively("audio/x-monkeys-audio"))
	}

	/** An allowlist, so a video type reaching this by mistake is refused too. */
	@Test
	fun `a video type is not an audio original`() {
		assertFalse(castPlaysNatively("video/mp4"))
	}
}
