package org.gaindrive.android.data

import org.gaindrive.android.data.model.Chapter
import org.junit.Assert.assertEquals
import org.junit.Assert.assertNull
import org.junit.Test

class TrackShareTest {

	private fun chapter(index: Int, start: Double, name: String = "") =
		Chapter(index = index, startSeconds = start, duration = 0, name = name)

	@Test
	fun `plain link when no start is chosen`() {
		assertEquals(
			"https://my.server.net/?track=42",
			trackShareUrl("https://my.server.net", "42"),
		)
	}

	@Test
	fun `a whole start is an integer, never 90 point 0`() {
		assertEquals(
			"https://my.server.net/?track=42&t=90",
			trackShareUrl("https://my.server.net", "42", 90.0),
		)
	}

	@Test
	fun `a chapter's fractional start travels verbatim`() {
		assertEquals(
			"http://host:4040/?track=7&t=6491.238",
			trackShareUrl("http://host:4040", "7", 6491.238),
		)
	}

	@Test
	fun `zero start means the plain link`() {
		assertEquals(
			"https://host/?track=1",
			trackShareUrl("https://host", "1", 0.0),
		)
	}

	@Test
	fun `the id keeps the server's spelling, encoded`() {
		assertEquals(
			"https://host/?track=007",
			trackShareUrl("https://host", "007"),
		)
		// Not an id any server issues today, but this side must not be the
		// one that breaks first.
		assertEquals(
			"https://host/?track=a%2Fb",
			trackShareUrl("https://host", "a/b"),
		)
	}

	// ── chapterAt ───────────────────────────────────────────────────────────

	private val concert = listOf(
		chapter(1, 0.0, "Intro"),
		chapter(2, 90.5),
		chapter(3, 6491.238, "Finale"),
	)

	@Test
	fun `inside the first chapter is the plain link`() {
		// It starts at 0, so "start at this chapter" would offer nothing.
		assertNull(chapterAt(concert, 30))
	}

	/**
	 * The regression the web dialog shipped: with start read as milliseconds,
	 * any position past a few seconds selected the last marker of a two-hour
	 * film and emitted its start off by a thousand.
	 */
	@Test
	fun `mid-recording picks the chapter being heard, not the last`() {
		assertEquals(90.5, chapterAt(concert, 120)!!.startSeconds, 0.0)
	}

	@Test
	fun `the last chapter only once the position is past its start`() {
		assertEquals("Finale", chapterAt(concert, 6500)!!.name)
		assertEquals(90.5, chapterAt(concert, 6491)!!.startSeconds, 0.0)
	}

	@Test
	fun `before the first marker, and with no markers, there is nothing`() {
		assertNull(chapterAt(listOf(chapter(1, 5.0, "A")), 2))
		assertNull(chapterAt(emptyList(), 100))
	}
}
