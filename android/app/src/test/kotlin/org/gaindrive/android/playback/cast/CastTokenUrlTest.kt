package org.gaindrive.android.playback.cast

import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Test

/**
 * Swapping this account's credentials out of a URL a television will fetch.
 *
 * Worth a test of its own for the same reason [CastPacingTest] is: both
 * failures are silent and remote. Leaving `u`/`t`/`s` on means the password
 * goes to the receiver anyway and nothing anywhere says so — the cast works
 * perfectly, which is exactly what makes it hard to notice. Dropping the id or
 * the pacing marker means a cast that fails minutes later for an unrelated
 * reason.
 *
 * Top-level `internal fun`, so these run with no Hilt and no `android.util`.
 */
class CastTokenUrlTest {

	private val ordinary =
		"http://host:4040/rest/stream.view?id=7&u=kasper&t=abc&s=def&v=1.16.1&c=gaindrive&f=json"

	@Test
	fun `the token is added`() {
		val url = withCastToken(ordinary, "deadbeef")
		assertTrue(url, url.contains("castToken=deadbeef"))
	}

	/** The whole point: none of the three may survive. */
	@Test
	fun `the account credentials are gone`() {
		val url = withCastToken(ordinary, "deadbeef")
		assertFalse(url, url.contains("u=kasper"))
		assertFalse(url, url.contains("t=abc"))
		assertFalse(url, url.contains("s=def"))
	}

	/**
	 * The id is what the grant is scoped against, so losing it turns a valid
	 * token into a refused one; `pace` is a separate question the token does
	 * not answer, and a paced URL that loses it dies ninety seconds in.
	 */
	@Test
	fun `the id and the pacing marker survive`() {
		val url = withCastToken(paced(ordinary), "deadbeef")
		assertTrue(url, url.contains("id=7"))
		assertTrue(url, url.contains("pace=true"))
	}

	/**
	 * Kept deliberately: the server ignores them on a grant-authed request and
	 * `c` is what names this client in its log.
	 */
	@Test
	fun `the protocol parameters survive`() {
		val url = withCastToken(ordinary, "deadbeef")
		assertTrue(url, url.contains("v=1.16.1"))
		assertTrue(url, url.contains("c=gaindrive"))
		assertTrue(url, url.contains("f=json"))
	}

	/**
	 * Applying it twice must not leave two, which would be a URL the server
	 * reads one arbitrary half of.
	 */
	@Test
	fun `a second application replaces rather than appends`() {
		val once = withCastToken(ordinary, "aaa")
		val twice = withCastToken(once, "bbb")
		val occurrences = twice.split("castToken=").size - 1
		assertTrue("$twice carried $occurrences tokens", occurrences == 1)
		assertTrue(twice, twice.contains("castToken=bbb"))
		assertFalse(twice, twice.contains("castToken=aaa"))
	}

	/** The path is not part of the query and must not be disturbed. */
	@Test
	fun `the path survives`() {
		assertTrue(withCastToken(ordinary, "x").contains("/rest/stream.view"))
	}

	/**
	 * Same concession [paced] makes: a malformed URL comes back unchanged, so
	 * a mangled server config still plays with the ordinary credentials rather
	 * than not at all.
	 */
	@Test
	fun `something that is not a url comes back unchanged`() {
		assertEquals("not a url", withCastToken("not a url", "deadbeef"))
	}
}
