package org.gaindrive.android.data.browse

import org.gaindrive.android.net.SongDto
import org.junit.Assert.assertEquals
import org.junit.Test

/** Turning an album folder's subdirectories into disc numbers. */
class FolderAlbumsTest {

	private fun song(title: String, disc: Int? = null, season: Int? = null) =
		SongDto(id = title, title = title, discNumber = disc, season = season)

	private fun dir(title: String) = SongDto(id = title, title = title, isDir = true)

	@Test
	fun `subfolders become disc numbers in order`() {
		val flat = flattenDiscs(
			listOf(
				listOf(song("Black Cow"), song("Aja")),
				listOf(song("Peg"), song("Home at Last")),
			)
		)
		assertEquals(4, flat.size)
		assertEquals(listOf(1, 1, 2, 2), flat.map { it.discNumber })
		// Order within a disc is the server's, untouched.
		assertEquals("Black Cow", flat[0].title)
		assertEquals("Peg", flat[2].title)
	}

	/** Lexical order gets this wrong at exactly ten discs, where it shows most. */
	@Test
	fun `natural order puts CD2 before CD10`() {
		val sorted = listOf(dir("CD10"), dir("CD2"), dir("CD1")).sortedWith(DISC_ORDER)
		assertEquals(listOf("CD1", "CD2", "CD10"), sorted.map { it.title })
	}

	@Test
	fun `natural order is case insensitive`() {
		val sorted = listOf(dir("disc 2"), dir("Disc 1")).sortedWith(DISC_ORDER)
		assertEquals(listOf("Disc 1", "disc 2"), sorted.map { it.title })
	}

	/**
	 * The point of folder mode: a set whose discs all claim to be disc 1 is
	 * precisely the library someone turned this on for.
	 */
	@Test
	fun `the folder's position overrides the track's own disc number`() {
		val flat = flattenDiscs(
			listOf(listOf(song("A", disc = 1)), listOf(song("B", disc = 1)))
		)
		assertEquals(listOf(1, 2), flat.map { it.discNumber })
	}

	/**
	 * `season` decides whether a group is headed "Series 2" or "Disc 2", and the
	 * rule is per folder: a disc whose tracks never had one must not acquire one
	 * from its position, or a two-CD album becomes a television series. A folder
	 * whose tracks all had one keeps it, so a show with an unnumbered `Specials`
	 * folder still heads that one group "Disc" — the same answer the server
	 * gives. Mixed input is therefore allowed to produce mixed headings.
	 */
	@Test
	fun `season is synthesised only for a folder whose tracks all had one`() {
		val flat = flattenDiscs(
			listOf(listOf(song("A"), song("B")), listOf(song("C", season = 9)))
		)
		assertEquals(listOf(null, null, 2), flat.map { it.season })
		assertEquals(listOf(1, 1, 2), flat.map { it.discNumber })
	}

	/** A real series read through its season folders keeps being one. */
	@Test
	fun `season survives when every track in the folder has one`() {
		val flat = flattenDiscs(
			listOf(
				listOf(song("Deserts", season = 1), song("Jungles", season = 1)),
				listOf(song("Islands", season = 2)),
			)
		)
		assertEquals(listOf(1, 1, 2), flat.map { it.season })
		assertEquals(listOf(1, 1, 2), flat.map { it.discNumber })
	}

	@Test
	fun `an empty folder contributes nothing but still consumes its number`() {
		val flat = flattenDiscs(listOf(emptyList(), listOf(song("A"))))
		assertEquals(1, flat.size)
		assertEquals(2, flat[0].discNumber)
	}
}
