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
		val two = listOf("a/1", "a/2")
		assertEquals(
			PinPhase.COMPLETE,
			pinPhaseOf(two, setOf("a/1", "a/2"), DownloadStates()),
		)
		assertEquals(
			PinPhase.RUNNING,
			pinPhaseOf(two, setOf("a/1"), DownloadStates(active = mapOf("a/2" to 2))),
		)
		assertEquals(0.5f, PinStatus(1, 2, PinPhase.RUNNING).fraction)
	}

	/**
	 * A pin whose track list has since been dropped would otherwise spin for
	 * ever, over something the user cannot act on.
	 */
	@Test
	fun `a pin covering nothing counts as done`() {
		assertEquals(PinPhase.COMPLETE, pinPhaseOf(emptyList(), emptySet(), DownloadStates()))
	}

	/**
	 * The symptom that prompted all of this: Media3 drops failed downloads out
	 * of its current-downloads set, so a failure used to be indistinguishable
	 * from nothing having happened.
	 */
	@Test
	fun `a failure with nothing still trying reports as failed`() {
		assertEquals(
			PinPhase.FAILED,
			pinPhaseOf(
				coveredKeys = listOf("a/1", "a/2"),
				storedKeys = setOf("a/1"),
				downloads = DownloadStates(failed = setOf("a/2")),
			),
		)
	}

	/** One track failing must not stop the rest of an album reporting progress. */
	@Test
	fun `a failure alongside a live download still reports as running`() {
		assertEquals(
			PinPhase.RUNNING,
			pinPhaseOf(
				coveredKeys = listOf("a/1", "a/2", "a/3"),
				storedKeys = emptySet(),
				downloads = DownloadStates(active = mapOf("a/1" to 2), failed = setOf("a/3")),
			),
		)
	}

	@Test
	fun `queued behind an unmet requirement reports as waiting`() {
		assertEquals(
			PinPhase.WAITING,
			pinPhaseOf(
				coveredKeys = listOf("a/1"),
				storedKeys = emptySet(),
				downloads = DownloadStates(active = mapOf("a/1" to 0), notMetRequirements = 1),
			),
		)
	}

	/**
	 * Nothing queued means nothing is being held back, so a metered connection
	 * must not make a finished album claim to be waiting.
	 */
	@Test
	fun `an unmet requirement with nothing queued does not report as waiting`() {
		assertEquals(
			PinPhase.COMPLETE,
			pinPhaseOf(
				coveredKeys = listOf("a/1"),
				storedKeys = setOf("a/1"),
				downloads = DownloadStates(notMetRequirements = 1),
			),
		)
	}

	/**
	 * The bug that made an album sit at "0 of 9" for ever: the downloads had
	 * finished, but gaindrive answers `stream.view` chunked with no
	 * `Content-Length`, so the cache never recorded a length to check against
	 * and could not vouch for a single track. Callers union the manager's own
	 * completed set in before asking for a phase.
	 */
	@Test
	fun `completed downloads count as stored even when the cache cannot say`() {
		val keys = listOf("a/1", "a/2")
		val states = DownloadStates(completed = setOf("a/1", "a/2"))
		assertEquals(
			PinPhase.COMPLETE,
			pinPhaseOf(keys, storedKeys = emptySet<String>() + states.completed, downloads = states),
		)
	}

	/**
	 * The instant after a tap, before the download manager has reported
	 * anything. Showing failure here would flash an error on every pin.
	 */
	@Test
	fun `a pin nothing has reported on yet reports as running`() {
		assertEquals(
			PinPhase.RUNNING,
			pinPhaseOf(listOf("a/1"), emptySet(), DownloadStates()),
		)
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
