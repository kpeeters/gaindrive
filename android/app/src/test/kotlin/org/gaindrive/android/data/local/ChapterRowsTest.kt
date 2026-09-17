package org.gaindrive.android.data.local

import org.gaindrive.android.data.model.ItemRef
import org.gaindrive.android.data.model.ServerId
import org.junit.Assert.assertEquals
import org.junit.Assert.assertTrue
import org.junit.Test

/**
 * Chapter rows back into the map the album screen reads.
 *
 * Worth testing without a database because this is where one album's rows are
 * split across the recordings they belong to, and getting it wrong would show
 * one long recording's markers against another.
 */
class ChapterRowsTest {

	private val server = ServerId("a")

	private fun row(songId: String, index: Int, start: Double, name: String) =
		ChapterEntity(
			serverId = server.value,
			songId = songId,
			chapterIndex = index,
			startSeconds = start,
			duration = 60,
			name = name,
		)

	@Test
	fun `rows are split by the recording they belong to`() {
		val grouped = groupChapters(
			server,
			listOf(
				row("set-1", 1, 0.0, "Opener"),
				row("set-1", 2, 300.5, "Second"),
				row("set-2", 1, 0.0, "Elsewhere"),
			),
		)

		assertEquals(
			setOf(ItemRef(server, "set-1"), ItemRef(server, "set-2")),
			grouped.keys,
		)
		assertEquals(2, grouped[ItemRef(server, "set-1")]?.size)
		assertEquals(listOf("Elsewhere"), grouped[ItemRef(server, "set-2")]?.map { it.name })
	}

	@Test
	fun `the order the query returned is kept`() {
		// The DAO orders by songId then chapterIndex, so nothing here re-sorts.
		// A list that came back out of order would seek to the wrong place.
		val grouped = groupChapters(
			server,
			listOf(
				row("set-1", 1, 0.0, "First"),
				row("set-1", 2, 120.0, "Second"),
				row("set-1", 3, 240.0, "Third"),
			),
		)

		assertEquals(listOf(1, 2, 3), grouped.values.single().map { it.index })
		assertEquals(listOf(0.0, 120.0, 240.0), grouped.values.single().map { it.startSeconds })
	}

	@Test
	fun `a recording with no rows is simply absent`() {
		// "In the map" and "has chapters" have to stay the same question; every
		// caller treats them as one.
		assertTrue(groupChapters(server, emptyList()).isEmpty())
	}

	@Test
	fun `the millisecond precision of a start survives the round trip`() {
		// Kept as sent rather than rounded, per Chapter.startSeconds: a client
		// that reads a list and writes it back has to be a fixed point.
		val grouped = groupChapters(server, listOf(row("set-1", 1, 301.237, "Exact")))

		assertEquals(301.237, grouped.values.single().single().startSeconds, 0.0)
		assertEquals(301_237L, grouped.values.single().single().startMs)
	}
}
