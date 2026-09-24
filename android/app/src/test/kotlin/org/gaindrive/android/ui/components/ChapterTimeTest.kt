package org.gaindrive.android.ui.components

import org.junit.Assert.assertEquals
import org.junit.Test

/**
 * A *position* inside a recording, which is not the same thing as a length -
 * see the note on [formatChapterTime]. The hour boundary is the whole point:
 * [formatDuration] has no rollover, so it names the 90-minute mark of a concert
 * "90:00".
 */
class ChapterTimeTest {

	@Test
	fun `zero is a real position and is printed`() {
		// formatDuration returns "" here, which is right for a length and wrong
		// for a position: the first marker is usually at 0:00.
		assertEquals("0:00", formatChapterTime(0.0))
	}

	@Test
	fun `below an hour is minutes and seconds`() {
		assertEquals("0:59", formatChapterTime(59.0))
		assertEquals("1:00", formatChapterTime(60.0))
		assertEquals("59:59", formatChapterTime(3599.0))
	}

	@Test
	fun `an hour rolls over rather than accumulating minutes`() {
		assertEquals("1:00:00", formatChapterTime(3600.0))
		assertEquals("1:30:00", formatChapterTime(5400.0))
		assertEquals("2:01:05", formatChapterTime(7265.0))
	}

	@Test
	fun `fractions floor, matching the web client`() {
		assertEquals("0:13", formatChapterTime(13.9))
	}

	@Test
	fun `a negative position cannot be produced but is not printed as one`() {
		assertEquals("0:00", formatChapterTime(-5.0))
	}
}
