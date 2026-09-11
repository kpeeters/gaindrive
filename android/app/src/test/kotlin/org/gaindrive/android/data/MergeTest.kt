package org.gaindrive.android.data

import org.gaindrive.android.data.model.Artist
import org.gaindrive.android.data.model.ArtistIndex
import org.gaindrive.android.data.model.ItemRef
import org.gaindrive.android.data.model.ServerId
import org.junit.Assert.assertEquals
import org.junit.Test

/**
 * The category half of the merged library list. The artist half is
 * [mergeArtistIndexes], exercised through the repository since forever; this
 * pins what [mergeCategories] adds on top of it.
 */
class MergeTest {

	private fun artist(server: String, id: String, name: String) = Artist(
		ref = ItemRef(ServerId(server), id),
		name = name,
		albumCount = 1,
		coverArt = null,
		starredAt = null,
	)

	private fun bucket(label: String, vararg artists: Artist) =
		ArtistIndex(label, artists.toList())

	@Test
	fun `categories flatten across buckets and sort alphabetically`() {
		val merged = mergeCategories(
			listOf(
				listOf(
					bucket("F", artist("a", "1", "Film")),
					bucket("S", artist("a", "2", "Series")),
				),
			)
		)
		assertEquals(listOf("Film", "Series"), merged.map { it.name })
	}

	/**
	 * The explicit sort is load-bearing: mergeArtistIndexes short-circuits a
	 * single server and returns its own bucket order, which under one header
	 * would read as no order at all.
	 */
	@Test
	fun `a single server's categories still come out alphabetical`() {
		val merged = mergeCategories(
			listOf(
				listOf(
					bucket("S", artist("a", "2", "Series")),
					bucket("D", artist("a", "3", "Documentary")),
					bucket("F", artist("a", "1", "Film")),
				),
			)
		)
		assertEquals(listOf("Documentary", "Film", "Series"), merged.map { it.name })
	}

	/** The same section on two servers is one row carrying both refs. */
	@Test
	fun `same-named categories collapse across servers`() {
		val merged = mergeCategories(
			listOf(
				listOf(bucket("F", artist("a", "1", "Film"))),
				listOf(bucket("F", artist("b", "9", "film"))),
			)
		)
		assertEquals(1, merged.size)
		// The first contributor's spelling and ref win, registry order
		// deciding as it does everywhere else.
		assertEquals("Film", merged[0].name)
		assertEquals(
			listOf(ItemRef(ServerId("a"), "1"), ItemRef(ServerId("b"), "9")),
			merged[0].refs,
		)
	}
}
