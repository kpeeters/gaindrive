package org.gaindrive.android.ui.browse

import org.gaindrive.android.data.model.Chapter
import org.gaindrive.android.data.model.ItemRef
import org.gaindrive.android.data.model.ServerId
import org.gaindrive.android.data.model.Song
import org.junit.Assert.assertEquals
import org.junit.Assert.assertTrue
import org.junit.Test

/**
 * How an album listing is laid out once a chaptered recording stands in for
 * itself. Pure, so every rule here is checked without a screen.
 */
class AlbumRowsTest {

	private val server = ServerId("s1")

	private fun song(
		id: String,
		title: String = "Track $id",
		track: Int? = null,
		disc: Int? = null,
		season: Int? = null,
	) = Song(
		ref = ItemRef(server, id),
		title = title,
		artistName = "Pink Floyd",
		albumTitle = "Pompeii",
		albumRef = ItemRef(server, "12"),
		track = track,
		discNumber = disc,
		year = null,
		duration = 300,
		bitRate = 0,
		suffix = null,
		contentType = null,
		sizeBytes = 0,
		coverArt = null,
		starredAt = null,
		season = season,
	)

	private fun markers(vararg names: String) =
		names.mapIndexed { i, name ->
			Chapter(index = i + 1, startSeconds = i * 100.0, duration = 100, name = name)
		}

	@Test
	fun `no chapters produces exactly the listing this screen always drew`() {
		val songs = listOf(song("1", track = 1), song("2", track = 2))
		val rows = albumListRows(songs, emptyMap())

		assertEquals(2, rows.size)
		assertTrue(rows.all { it is AlbumListRow.Track })
		assertTrue(rows.all { it.headings.isEmpty() })
		assertEquals(listOf(1, 2), rows.map { (it as AlbumListRow.Track).number })
	}

	@Test
	fun `a chaptered recording is replaced by its markers`() {
		val concert = song("1", title = "Live at Pompeii")
		val rows = albumListRows(
			listOf(concert),
			mapOf(concert.ref to markers("Echoes", "Careful With That Axe")),
		)

		// Its own row is gone: the markers are what the folder holds, and a row
		// naming the file would be a duplicate of all of them.
		assertTrue(rows.none { it is AlbumListRow.Track })
		assertEquals(listOf("Echoes", "Careful With That Axe"),
			rows.map { (it as AlbumListRow.Marker).chapter.name })
		// Every marker plays the same recording, from its own point.
		assertTrue(rows.all { (it as AlbumListRow.Marker).queueIndex == 0 })
	}

	@Test
	fun `one chaptered recording needs no heading naming it`() {
		val concert = song("1", title = "Live at Pompeii")
		val rows = albumListRows(listOf(concert), mapOf(concert.ref to markers("Echoes")))
		assertTrue(rows.single().headings.isEmpty())
	}

	@Test
	fun `two chaptered recordings each get a heading`() {
		// The rule multiDisc follows, for the same reason: a heading says which
		// group a row belongs to, so one group needs none.
		val first = song("1", title = "Live at Pompeii")
		val second = song("2", title = "The Encore")
		val rows = albumListRows(
			listOf(first, second),
			mapOf(first.ref to markers("Echoes"), second.ref to markers("Astronomy")),
		)
		assertEquals(listOf("Live at Pompeii"), rows[0].headings)
		assertEquals(listOf("The Encore"), rows[1].headings)
	}

	@Test
	fun `a disc heading stacks onto the first marker of a chaptered recording`() {
		// The case a naive port loses: the recording emits no row of its own, so
		// there is nothing left to hang "Disc 2" on and both headings have to
		// travel with its first marker.
		val plain = song("1", disc = 1, track = 1)
		val concert = song("2", title = "Live at Pompeii", disc = 2)
		val rows = albumListRows(
			listOf(plain, concert),
			mapOf(concert.ref to markers("Echoes", "Astronomy")),
		)
		assertTrue(rows[0].headings.isEmpty())
		assertEquals(listOf("Disc 2"), rows[1].headings)
		assertTrue(rows[2].headings.isEmpty())
	}

	@Test
	fun `a season heads its group as a series`() {
		val first = song("1", disc = 1, track = 1, season = 1)
		val second = song("2", disc = 2, track = 1, season = 2)
		val rows = albumListRows(listOf(first, second), emptyMap())
		assertEquals(listOf("Series 1"), rows[0].headings)
		assertEquals(listOf("Series 2"), rows[1].headings)
	}

	@Test
	fun `untagged track numbers fall back to position`() {
		val rows = albumListRows(listOf(song("1"), song("2"), song("3")), emptyMap())
		assertEquals(listOf(1, 2, 3), rows.map { (it as AlbumListRow.Track).number })
	}

	@Test
	fun `every row is uniquely keyed`() {
		val plain = song("1", track = 1)
		val concert = song("2", title = "Live at Pompeii")
		val rows = albumListRows(
			listOf(plain, concert),
			mapOf(concert.ref to markers("Echoes", "Astronomy")),
		)
		assertEquals(rows.size, rows.mapTo(mutableSetOf()) { it.key }.size)
	}
}
