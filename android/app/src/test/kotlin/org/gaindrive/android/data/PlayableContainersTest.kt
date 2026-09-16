package org.gaindrive.android.data

import org.gaindrive.android.playback.MEDIA3_CONTAINERS
import org.gaindrive.android.playback.demuxedLocally
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Test

/**
 * The declaration that stops the server remuxing a container we demux
 * ourselves, and — the half worth testing — its absence everywhere else.
 *
 * The same reasoning as `CastPacingTest`: the failure is silent and remote. A
 * cast URL that carries `playable` gets the receiver a `LOAD`
 * announcing `video/mp4` followed by Matroska, which it refuses outright. The
 * film never starts and nothing on the phone says why. The obvious refactor —
 * moving the declaration into `StreamUrls.forVideo`'s default "because both
 * callers want it" — is exactly that bug, and `the cast route declares nothing`
 * below is what stands in its way.
 */
class PlayableContainersTest {

	@Test
	fun `a declared set becomes one sorted comma list`() {
		assertEquals(
			mapOf("id" to "7", "playable" to "avi,mkv,mov"),
			videoStreamParams("7", setOf("mov", "mkv", "avi")),
		)
	}

	/**
	 * A `Set`'s iteration order is not a promise, and these end up in OkHttp's
	 * cache key and in the server's log, so one request must build exactly one
	 * URL however the set was assembled.
	 */
	@Test
	fun `the order of the set does not reach the url`() {
		assertEquals(
			videoStreamParams("7", setOf("mkv", "avi")),
			videoStreamParams("7", setOf("avi", "mkv")),
		)
	}

	/**
	 * The cast route passes no set at all, and this is the assertion that
	 * catches it being given one. `CastUrls.forVideo` itself needs Hilt, so
	 * the guard sits at the seam both routes share.
	 */
	@Test
	fun `the cast route declares nothing`() {
		assertEquals(mapOf("id" to "7"), videoStreamParams("7", emptySet()))
	}

	/**
	 * A DVD titleset is one stream split across numbered VOBs and the stored
	 * path names only the first, so serving it untouched hands back twenty
	 * minutes of a two-hour film. The server refuses the declaration; this
	 * records the same rule on the side that would make it.
	 */
	@Test
	fun `vob is never declared`() {
		assertFalse(MEDIA3_CONTAINERS.contains("vob"))
		assertFalse(demuxedLocally("vob"))
		assertFalse(demuxedLocally("VOB"))
	}

	/** media3 has no ASF extractor, so claiming one would be a black player. */
	@Test
	fun `wmv is never declared`() {
		assertFalse(demuxedLocally("wmv"))
	}

	/** `songs.suffix` is lowercased server-side, but nothing here relies on it. */
	@Test
	fun `the suffix test is case insensitive and null safe`() {
		assertTrue(demuxedLocally("mkv"))
		assertTrue(demuxedLocally("MKV"))
		assertFalse(demuxedLocally(null))
		assertFalse(demuxedLocally(""))
	}

	/**
	 * mp4 and webm are served untouched to everyone already, so declaring them
	 * would say nothing. Keeping them out is what makes the set mean "more
	 * than a browser takes".
	 */
	@Test
	fun `containers the server already serves directly are absent`() {
		assertFalse(demuxedLocally("mp4"))
		assertFalse(demuxedLocally("webm"))
		assertFalse(demuxedLocally("m4v"))
	}
}
