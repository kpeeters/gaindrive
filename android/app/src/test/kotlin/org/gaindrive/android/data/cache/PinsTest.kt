package org.gaindrive.android.data.cache

import org.gaindrive.android.data.model.Album
import org.gaindrive.android.data.model.ItemRef
import org.gaindrive.android.data.model.ServerId
import org.gaindrive.android.data.model.Song
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertNull
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

	private fun album(ref: ItemRef, cover: ItemRef?, artist: ItemRef?) = Album(
		ref = ref,
		title = "An Album",
		artistName = "An Artist",
		artistRef = artist,
		songCount = 2,
		duration = 300,
		year = null,
		genre = null,
		coverArt = cover,
		starredAt = null,
	)

	private fun song(ref: ItemRef, cover: ItemRef?) = Song(
		ref = ref,
		title = "A Track",
		artistName = "An Artist",
		albumTitle = "An Album",
		albumRef = null,
		track = null,
		discNumber = null,
		year = null,
		duration = 150,
		bitRate = 320,
		suffix = null,
		contentType = null,
		sizeBytes = 1000,
		coverArt = cover,
		starredAt = null,
	)

	private val fetching = TrackDownload(TrackDownloadState.DOWNLOADING, percent = 40f)

	/** Queued tracks have no figure yet, which is what -1 means here. */
	private val waiting = TrackDownload(TrackDownloadState.QUEUED, percent = -1f)

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
			pinPhaseOf(two, setOf("a/1"), DownloadStates(active = mapOf("a/2" to fetching))),
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
				downloads = DownloadStates(
					active = mapOf("a/1" to fetching),
					failed = setOf("a/3"),
				),
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
				downloads = DownloadStates(
					active = mapOf("a/1" to waiting),
					notMetRequirements = 1,
				),
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

	/**
	 * A queued track has no figure to draw, and a determinate ring pinned at
	 * zero looks stalled rather than starting — so the row falls back to a
	 * spinner until there is something real to show.
	 */
	@Test
	fun `only a track actually being fetched reports a fraction`() {
		assertNull(waiting.knownFraction)
		assertNull(TrackDownload(TrackDownloadState.DOWNLOADING, percent = -1f).knownFraction)
		assertEquals(0.4f, fetching.knownFraction)
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

	@Test
	fun `a collection counts as stored once every track of it is here`() {
		val stored = collectionsFullyStored(
			membership = mapOf(
				"a/10" to listOf("a/1", "a/2"),
				"a/11" to listOf("a/3", "a/4"),
			),
			here = setOf("a/1", "a/2", "a/3"),
		)
		assertEquals(setOf("a/10"), stored)
	}

	/**
	 * The one that matters. The mirror only holds the tracks of collections
	 * visited while online, so an album nobody has opened has no members at all
	 * — and `containsAll` over an empty list is vacuously true. Without the
	 * guard every album in a fresh library would claim to be downloaded.
	 *
	 * Note this is the opposite answer to `a pin covering nothing counts as
	 * done` above, and deliberately: there the user asked for the thing, so the
	 * question is whether their request is outstanding. Here nobody asked, and
	 * the question is whether the bytes are present.
	 */
	@Test
	fun `a collection the mirror knows no tracks of is not stored`() {
		assertTrue(
			collectionsFullyStored(
				membership = mapOf("a/10" to emptyList()),
				here = setOf("a/1", "a/2"),
			).isEmpty()
		)
	}

	/**
	 * A film in an album's folder would otherwise make it permanently
	 * incomplete: a video is not downloaded at all unless it is being played
	 * for its soundtrack, so it can never join the stored set.
	 */
	@Test
	fun `a video counts only when videos are played as audio`() {
		assertFalse(covered(isVideo = true, audioOnly = false))
		assertTrue(covered(isVideo = true, audioOnly = true))
		assertTrue(covered(isVideo = false, audioOnly = false))
	}

	@Test
	fun `an album pin covers its cover, its artist and nothing twice`() {
		val albumRef = ref(serverA, "10")
		val cover = ref(serverA, "cover-10")
		val artist = ref(serverA, "artist-3")
		// The usual case: every track carries the album's own cover art id, so
		// an album's worth of tracks is one picture, not twelve.
		val tracks = listOf(song(ref(serverA, "1"), cover), song(ref(serverA, "2"), cover))

		val art = artRefsOf(Pin(albumRef, PinKind.ALBUM), album(albumRef, cover, artist), tracks)

		assertEquals(listOf(cover, artist), art)
	}

	@Test
	fun `an album missing from the mirror falls back to its own ref`() {
		// The window where a server has been removed or its browse mode
		// changed: the rows are gone but the pin is still there. A cover art id
		// is a folder id, so the pin's own ref is a usable stand-in, and
		// covering nothing at all would let the files be swept.
		val albumRef = ref(serverA, "10")

		val art = artRefsOf(Pin(albumRef, PinKind.ALBUM), album = null, songs = emptyList())

		assertEquals(listOf(albumRef), art)
	}

	@Test
	fun `a playlist pin covers one picture per distinct album`() {
		val first = ref(serverA, "cover-10")
		val second = ref(serverB, "cover-20")
		val tracks = listOf(
			song(ref(serverA, "1"), first),
			song(ref(serverA, "2"), first),
			song(ref(serverB, "3"), second),
		)

		val art = artRefsOf(Pin(ref(serverA, "pl-1"), PinKind.PLAYLIST), album = null, songs = tracks)

		assertEquals(listOf(first, second), art)
	}

	@Test
	fun `a song pin covers exactly one picture`() {
		val cover = ref(serverA, "cover-10")
		val songRef = ref(serverA, "1")

		val art = artRefsOf(Pin(songRef, PinKind.SONG), album = null, songs = listOf(song(songRef, cover)))

		assertEquals(listOf(cover), art)
	}
}
