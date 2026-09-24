package org.gaindrive.android.data

import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Test

/**
 * The URIs the server's chooser page actually composes, plus the shapes a
 * hostile or confused sender could put in a VIEW intent — the filter admits
 * anything under the scheme, so the parser is the boundary.
 */
class TrackLinkTest {

	@Test
	fun `a plain track link parses`() {
		assertEquals(
			TrackLink("my.server.net", "/", "42", 0),
			parseTrackLink("gaindrive://my.server.net/?track=42"),
		)
	}

	@Test
	fun `a position becomes milliseconds`() {
		assertEquals(
			TrackLink("my.server.net", "/", "42", 90_000),
			parseTrackLink("gaindrive://my.server.net/?track=42&t=90"),
		)
	}

	@Test
	fun `port and proxy subpath survive`() {
		assertEquals(
			TrackLink("192.168.1.5:4040", "/music/", "7", 0),
			parseTrackLink("gaindrive://192.168.1.5:4040/music/?track=7"),
		)
	}

	@Test
	fun `the scheme is case-insensitive, as schemes are`() {
		assertEquals("42", parseTrackLink("GainDrive://host/?track=42")?.trackId)
	}

	@Test
	fun `the id keeps the server's spelling`() {
		// Not parsed to a number: "007" must reach getSong as sent.
		assertEquals("007", parseTrackLink("gaindrive://host/?track=007")?.trackId)
	}

	@Test
	fun `junk positions mean zero`() {
		assertEquals(0L, parseTrackLink("gaindrive://host/?track=1&t=")?.positionMs)
		assertEquals(0L, parseTrackLink("gaindrive://host/?track=1&t=abc")?.positionMs)
		assertEquals(0L, parseTrackLink("gaindrive://host/?track=1&t=-5")?.positionMs)
	}

	@Test
	fun `a fractional position keeps its milliseconds`() {
		assertEquals(12_500L, parseTrackLink("gaindrive://host/?track=1&t=12.5")?.positionMs)
	}

	@Test
	fun `not a track link is null, not an error`() {
		assertNull(parseTrackLink(null))
		assertNull(parseTrackLink("https://my.server.net/?track=42"))
		assertNull(parseTrackLink("gaindrive://host/"))
		assertNull(parseTrackLink("gaindrive://host/?track="))
		assertNull(parseTrackLink("gaindrive://?track=42"))
	}

	@Test
	fun `a fragment does not smuggle parameters`() {
		assertNull(parseTrackLink("gaindrive://host/#?track=42"))
	}

	// ── Which configured server a link belongs to ───────────────────────────

	@Test
	fun `host matching ignores case, path is exact or below`() {
		val link = parseTrackLink("gaindrive://My.Server.NET/?track=1")!!
		assertTrue(serverMatchesLink("https://my.server.net", link))
		assertTrue(serverMatchesLink("http://my.server.net/", link))
		assertFalse(serverMatchesLink("https://other.server.net", link))
	}

	@Test
	fun `a port separates two servers on one host`() {
		val link = parseTrackLink("gaindrive://host:4040/?track=1")!!
		assertTrue(serverMatchesLink("http://host:4040", link))
		assertFalse(serverMatchesLink("http://host:5050", link))
		assertFalse(serverMatchesLink("http://host", link))
	}

	@Test
	fun `a subpath mount matches its own links and not its neighbour's`() {
		val music = parseTrackLink("gaindrive://host/music/?track=1")!!
		assertTrue(serverMatchesLink("https://host/music", music))
		assertFalse(serverMatchesLink("https://host/films", music))
		// A root-mounted server owns every path under its host.
		assertTrue(serverMatchesLink("https://host", music))
	}
}
