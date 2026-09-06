package org.gaindrive.android.data.model

import org.junit.Assert.assertEquals
import org.junit.Assert.assertNull
import org.junit.Test

/**
 * The marker-navigation rules, which are the whole of what the chapter panel
 * does beyond drawing a list. Ported from `web/app.js` rather than reinvented,
 * so these pin the two clients to the same answers.
 */
class ChapterNavTest {

	private fun chapter(index: Int, seconds: Double, name: String = "") =
		Chapter(index = index, startSeconds = seconds, duration = 0, name = name)

	/** 0:00, 1:40, 3:20. */
	private val list = listOf(
		chapter(1, 0.0),
		chapter(2, 100.0),
		chapter(3, 200.0),
	)

	@Test
	fun `nothing is current before the first marker`() {
		assertEquals(-1, listOf(chapter(1, 30.0)).currentAt(0))
	}

	@Test
	fun `a marker becomes current exactly at its start`() {
		assertEquals(1, list.currentAt(100_000))
	}

	@Test
	fun `a marker becomes current inside the tolerance`() {
		// Seeking lands a few milliseconds short — the player rounds, and a
		// re-encoded stream starts at the nearest keyframe — so without this the
		// list would highlight the previous song for a moment after a jump.
		assertEquals(1, list.currentAt(100_000 - CHAPTER_TOLERANCE_MS))
	}

	@Test
	fun `a marker is not yet current just outside the tolerance`() {
		assertEquals(0, list.currentAt(100_000 - CHAPTER_TOLERANCE_MS - 1))
	}

	@Test
	fun `the last marker stays current to the end`() {
		assertEquals(2, list.currentAt(9_999_000))
	}

	@Test
	fun `an empty list has no current marker`() {
		assertEquals(-1, emptyList<Chapter>().currentAt(1_000))
	}

	@Test
	fun `next is the following marker`() {
		assertEquals(3, list.nextAfter(150_000)?.index)
	}

	@Test
	fun `next is null past the last marker`() {
		assertNull(list.nextAfter(250_000))
	}

	@Test
	fun `next skips a marker that is only just ahead`() {
		// Same tolerance as currentAt, and it must be: otherwise pressing next
		// immediately after landing on a marker would go to the one you are on.
		assertNull(listOf(chapter(1, 100.0)).nextAfter(100_000 - CHAPTER_TOLERANCE_MS))
	}

	@Test
	fun `previous restarts the current marker when well into it`() {
		assertEquals(100_000L, list.previousTargetMs(100_000 + CHAPTER_RESTART_MS + 1))
	}

	@Test
	fun `previous steps back when barely into the current marker`() {
		// The behaviour every physical transport has, and the reason the button
		// can be pressed twice to go back a song.
		assertEquals(0L, list.previousTargetMs(100_000 + 500))
	}

	@Test
	fun `previous from inside the first marker restarts it`() {
		// There is nothing before it to step back to, so it must not fall
		// through to a negative index.
		assertEquals(0L, list.previousTargetMs(500))
	}

	@Test
	fun `previous before the first marker seeks to the start`() {
		// Never inert: a recording whose first marker is a minute in still has
		// a sensible answer for "previous".
		assertEquals(0L, listOf(chapter(1, 60.0)).previousTargetMs(1_000))
	}

	@Test
	fun `an empty name is replaced only for display`() {
		// The server reports a bare marker as bare on purpose, so a client that
		// saved back what it read cannot write a placeholder into a line
		// somebody left blank. The placeholder belongs here and nowhere else.
		assertEquals("", chapter(4, 0.0).name)
		assertEquals("Chapter 4", chapter(4, 0.0).displayName)
		assertEquals("Encore", chapter(4, 0.0, "Encore").displayName)
	}

	@Test
	fun `start is carried to the millisecond`() {
		assertEquals(214_500L, chapter(1, 214.5).startMs)
	}
}
