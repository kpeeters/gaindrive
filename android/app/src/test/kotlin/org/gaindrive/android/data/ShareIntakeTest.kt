package org.gaindrive.android.data

import org.junit.Assert.assertEquals
import org.junit.Assert.assertNull
import org.junit.Test

/**
 * The shapes real apps actually put in `EXTRA_TEXT`. Getting this wrong is not
 * a crash: the server refuses whatever is handed to it and the user sees a
 * fetch that will not start, with the URL sitting right there on screen looking
 * correct.
 */
class ShareIntakeTest {

	@Test
	fun `a bare URL is taken whole`() {
		assertEquals(
			"https://www.youtube.com/watch?v=dQw4w9WgXcQ",
			extractSharedUrl("https://www.youtube.com/watch?v=dQw4w9WgXcQ"),
		)
	}

	/** What the YouTube app sends: the video's title, a newline, then the link. */
	@Test
	fun `a title above the link is skipped`() {
		assertEquals(
			"https://youtu.be/dQw4w9WgXcQ",
			extractSharedUrl("Never Gonna Give You Up\nhttps://youtu.be/dQw4w9WgXcQ"),
		)
	}

	@Test
	fun `a link inside a sentence keeps its query but loses the full stop`() {
		assertEquals(
			"https://example.com/a?b=c",
			extractSharedUrl("Listen to https://example.com/a?b=c."),
		)
	}

	/** A closing bracket that has its opener inside the URL belongs to it. */
	@Test
	fun `balanced brackets survive`() {
		assertEquals(
			"https://en.wikipedia.org/wiki/Kes_(film)",
			extractSharedUrl("https://en.wikipedia.org/wiki/Kes_(film)"),
		)
	}

	@Test
	fun `an unbalanced closing bracket is dropped`() {
		assertEquals(
			"https://example.com/a",
			extractSharedUrl("(see https://example.com/a)"),
		)
	}

	@Test
	fun `the first of several links wins`() {
		assertEquals(
			"https://example.com/one",
			extractSharedUrl("https://example.com/one and https://example.com/two"),
		)
	}

	@Test
	fun `http is accepted as well as https`() {
		assertEquals("http://nas.local/x", extractSharedUrl("http://nas.local/x"))
	}

	@Test
	fun `text with no link yields nothing`() {
		assertNull(extractSharedUrl("Remember to buy milk"))
	}

	/**
	 * The server refuses anything but http(s), and refusing it here means the
	 * panel never opens on something that could not have worked.
	 */
	@Test
	fun `other schemes are not URLs for this purpose`() {
		assertNull(extractSharedUrl("file:///etc/shadow"))
		assertNull(extractSharedUrl("ftp://example.com/x"))
	}

	@Test
	fun `a bare scheme is not a URL`() {
		assertNull(extractSharedUrl("https://"))
	}

	@Test
	fun `null and blank are handled`() {
		assertNull(extractSharedUrl(null))
		assertNull(extractSharedUrl("   "))
	}
}
