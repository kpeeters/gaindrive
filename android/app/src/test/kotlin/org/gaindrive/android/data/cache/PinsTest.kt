package org.gaindrive.android.data.cache

import org.gaindrive.android.data.model.ItemRef
import org.gaindrive.android.data.model.ServerId
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Test

/**
 * The rule that decides what survives eviction. Worth testing on its own: it is
 * the one place where getting it wrong deletes something the user asked to
 * keep, and the failure would only show up offline, long afterwards.
 */
class PinsTest {

	private val serverA = ServerId("a")
	private val serverB = ServerId("b")

	private fun ref(server: ServerId, id: String) = ItemRef(server, id)

	@Test
	fun `a song pin protects exactly itself`() {
		val song = ref(serverA, "1")
		val keys = expandPins(
			pins = listOf(Pin(song, PinKind.SONG)),
			albumSongs = emptyMap(),
			playlistSongs = emptyMap(),
		).allKeys
		assertEquals(setOf(song.encode()), keys)
	}

	@Test
	fun `an album pin protects its tracks, not the album id`() {
		val album = ref(serverA, "10")
		val tracks = listOf(ref(serverA, "1"), ref(serverA, "2"))
		val keys = expandPins(
			pins = listOf(Pin(album, PinKind.ALBUM)),
			albumSongs = mapOf(album to tracks),
			playlistSongs = emptyMap(),
		).allKeys
		assertEquals(tracks.map { it.encode() }.toSet(), keys)
	}

	/** Pinning something never browsed cannot know what it holds yet. */
	@Test
	fun `an album pin with no stored track list protects nothing`() {
		val keys = expandPins(
			pins = listOf(Pin(ref(serverA, "10"), PinKind.ALBUM)),
			albumSongs = emptyMap(),
			playlistSongs = emptyMap(),
		).allKeys
		assertTrue(keys.isEmpty())
	}

	@Test
	fun `overlapping pins produce one key each`() {
		val song = ref(serverA, "1")
		val album = ref(serverA, "10")
		val keys = expandPins(
			pins = listOf(Pin(song, PinKind.SONG), Pin(album, PinKind.ALBUM)),
			albumSongs = mapOf(album to listOf(song, ref(serverA, "2"))),
			playlistSongs = emptyMap(),
		).allKeys
		assertEquals(setOf(song.encode(), ref(serverA, "2").encode()), keys)
	}

	/**
	 * The union is what eviction needs; the breakdown is what the download
	 * indicator needs, and an album pin has to keep its own tracks together for
	 * "3 of 12" to mean anything.
	 */
	@Test
	fun `coverage stays broken down per pin`() {
		val album = ref(serverA, "10")
		val song = ref(serverA, "99")
		val coverage = expandPins(
			pins = listOf(Pin(album, PinKind.ALBUM), Pin(song, PinKind.SONG)),
			albumSongs = mapOf(album to listOf(ref(serverA, "1"), ref(serverA, "2"))),
			playlistSongs = emptyMap(),
		)

		assertEquals(2, coverage.byPin[album.encode()]?.size)
		assertEquals(listOf(song.encode()), coverage.byPin[song.encode()])
		assertEquals(3, coverage.allKeys.size)
	}

	@Test
	fun `a pin is only complete once every track it covers is stored`() {
		assertTrue(PinStatus(stored = 2, total = 2, active = false).complete)
		assertFalse(PinStatus(stored = 1, total = 2, active = true).complete)
		assertEquals(0.5f, PinStatus(stored = 1, total = 2, active = true).fraction)
	}

	/**
	 * A pin whose track list has since been dropped would otherwise spin for
	 * ever, over something the user cannot act on.
	 */
	@Test
	fun `a pin covering nothing counts as done`() {
		assertTrue(PinStatus(stored = 0, total = 0, active = false).complete)
	}

	/** The whole reason keys are composite: id 1 on two servers is two tracks. */
	@Test
	fun `same id on different servers stays distinct`() {
		val keys = expandPins(
			pins = listOf(
				Pin(ref(serverA, "1"), PinKind.SONG),
				Pin(ref(serverB, "1"), PinKind.SONG),
			),
			albumSongs = emptyMap(),
			playlistSongs = emptyMap(),
		).allKeys
		assertEquals(2, keys.size)
	}

	@Test
	fun `a playlist pin follows its current membership`() {
		val playlist = ref(serverA, "p1")
		val first = listOf(ref(serverA, "1"))
		val later = listOf(ref(serverA, "1"), ref(serverA, "9"))

		val before = expandPins(
			pins = listOf(Pin(playlist, PinKind.PLAYLIST)),
			albumSongs = emptyMap(),
			playlistSongs = mapOf(playlist to first),
		).allKeys
		val after = expandPins(
			pins = listOf(Pin(playlist, PinKind.PLAYLIST)),
			albumSongs = emptyMap(),
			playlistSongs = mapOf(playlist to later),
		).allKeys

		assertEquals(1, before.size)
		assertTrue(ref(serverA, "9").encode() in after)
	}
}
