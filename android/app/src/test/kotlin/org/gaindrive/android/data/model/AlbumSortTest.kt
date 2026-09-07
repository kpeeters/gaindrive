package org.gaindrive.android.data.model

import org.junit.Assert.assertEquals
import org.junit.Test

/** The two orders the albums screen offers. */
class AlbumSortTest {

	private val server = ServerId("server-a")

	private fun album(title: String, year: Int?) = Album(
		ref = ItemRef(server, title),
		title = title,
		artistName = "An Artist",
		artistRef = null,
		songCount = 1,
		duration = 1,
		year = year,
		genre = null,
		coverArt = null,
		starredAt = null,
	)

	private fun titles(sort: AlbumSort, vararg albums: Album) =
		albums.toList().sortedWith(sort.comparator).map { it.title }

	@Test
	fun `year order matches the server's, ties broken by title`() {
		assertEquals(
			listOf("Aja", "Gaucho", "Two Against Nature"),
			titles(
				AlbumSort.YEAR,
				album("Two Against Nature", 2000),
				album("Gaucho", 1980),
				album("Aja", 1977),
			),
		)
		assertEquals(
			listOf("Katy Lied", "The Royal Scam"),
			titles(
				AlbumSort.YEAR,
				album("The Royal Scam", 1976),
				album("Katy Lied", 1976),
			),
		)
	}

	/** SQLite sorts a NULL year first, and dropping the sort must not differ. */
	@Test
	fun `an album with no year sorts first by year`() {
		assertEquals(
			listOf("Unknown", "Aja"),
			titles(AlbumSort.YEAR, album("Aja", 1977), album("Unknown", null)),
		)
	}

	@Test
	fun `name order ignores case`() {
		assertEquals(
			listOf("aja", "Gaucho", "the royal scam"),
			titles(
				AlbumSort.NAME,
				album("Gaucho", 1980),
				album("the royal scam", 1976),
				album("aja", 1977),
			),
		)
	}

	/** Two editions of one title stay together and in year order. */
	@Test
	fun `name order breaks a tie by year`() {
		assertEquals(
			listOf(1977, 1999),
			listOf(album("Aja", 1999), album("Aja", 1977))
				.sortedWith(AlbumSort.NAME.comparator)
				.map { it.year },
		)
	}

	@Test
	fun `an unknown stored value falls back to the default`() {
		assertEquals(AlbumSort.YEAR, AlbumSort.DEFAULT)
		assertEquals(AlbumSort.YEAR, AlbumSort.parse(null))
		assertEquals(AlbumSort.YEAR, AlbumSort.parse("ARTIST"))
		assertEquals(AlbumSort.NAME, AlbumSort.parse("NAME"))
	}
}
