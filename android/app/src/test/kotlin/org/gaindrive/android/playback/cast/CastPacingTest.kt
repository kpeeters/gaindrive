package org.gaindrive.android.playback.cast

import org.junit.Assert.assertEquals
import org.junit.Assert.assertTrue
import org.junit.Test

/**
 * The marker that tells the server to deliver a stream at playback rate.
 *
 * Worth a test of its own because the failure it prevents is silent and
 * remote: a URL that reaches a receiver without `pace=true` plays for about
 * ninety seconds and stops, with the reason a minute of server log away. The
 * cases below are the three ways the string could be mangled - a lost query,
 * a lost path, a URL that is not one - each of which would look like the
 * parameter simply not working.
 */
class CastPacingTest {

	@Test
	fun `the marker is added`() {
		val url = paced("http://host:4040/rest/stream.view?id=7")
		assertTrue(url, url.contains("pace=true"))
	}

	/**
	 * The credentials and the id travel in the same query string, so appending
	 * has to preserve what is already there - a receiver handed a URL with the
	 * auth dropped gets a 401 rather than a track.
	 */
	@Test
	fun `the existing query and path survive`() {
		val original = "http://host:4040/rest/stream.view?id=7&u=kasper&t=abc&s=def"
		val url = paced(original)
		assertTrue(url, url.startsWith(original))
		assertTrue(url, url.endsWith("&pace=true"))
	}

	/** A URL with no query at all still gets one. */
	@Test
	fun `a url with no query gets the first parameter`() {
		assertEquals("http://host:4040/x?pace=true", paced("http://host:4040/x"))
	}

	/**
	 * The bridge publishes `http://` URLs it built itself, but the fallback
	 * paths hand over whatever a server config produced. Returning the input
	 * unchanged means a malformed one still plays unpaced rather than not at
	 * all.
	 */
	@Test
	fun `something that is not a url comes back unchanged`() {
		assertEquals("not a url", paced("not a url"))
	}
}
