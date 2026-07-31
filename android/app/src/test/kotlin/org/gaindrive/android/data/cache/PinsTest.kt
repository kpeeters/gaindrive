package org.gaindrive.android.data.cache

import org.gaindrive.android.data.model.ItemRef
import org.gaindrive.android.data.model.ServerId
import org.junit.Assert.assertEquals
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
		)
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
		)
		assertEquals(tracks.map { it.encode() }.toSet(), keys)
	}

	/** Pinning something never browsed cannot know what it holds yet. */
	@Test
	fun `an album pin with no stored track list protects nothing`() {
		val keys = expandPins(
			pins = listOf(Pin(ref(serverA, "10"), PinKind.ALBUM)),
			albumSongs = emptyMap(),
			playlistSongs = emptyMap(),
		)
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
		)
		assertEquals(setOf(song.encode(), ref(serverA, "2").encode()), keys)
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
		)
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
		)
		val after = expandPins(
			pins = listOf(Pin(playlist, PinKind.PLAYLIST)),
			albumSongs = emptyMap(),
			playlistSongs = mapOf(playlist to later),
		)

		assertEquals(1, before.size)
		assertTrue(ref(serverA, "9").encode() in after)
	}
}
