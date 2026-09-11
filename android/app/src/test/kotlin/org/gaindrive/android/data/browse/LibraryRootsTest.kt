package org.gaindrive.android.data.browse

import org.gaindrive.android.data.model.MusicRoot
import org.junit.Assert.assertEquals
import org.junit.Assert.assertNull
import org.junit.Test

/**
 * What one server is asked for the merged library list, and for uploads.
 *
 * The rule has to serve three kinds of server at once — one that names kinds of
 * root, one that has several untyped libraries, and one that has neither — so
 * most of these tests exist to pin the cases that must *not* change.
 */
class LibraryRootsTest {

	private fun typed(vararg names: Pair<String, String>) =
		names.mapIndexed { i, (name, type) -> MusicRoot("${i + 1}", name, type) }

	private fun untyped(vararg names: String) =
		names.mapIndexed { i, name -> MusicRoot("${i + 1}", name, null) }

	// ── Typed servers: one request per kind actually present ────────────

	/**
	 * The guarantee that makes the browse setting safe on a gaindrive server:
	 * it names kinds of root, so both halves are asked by kind whichever way
	 * the hierarchy is being read, and flipping the switch changes only the
	 * endpoints.
	 */
	@Test
	fun `a server naming both kinds is asked for each, in both modes`() {
		val roots = typed("music" to "artists", "movies" to "categories")
		for (byFolder in listOf(false, true)) {
			val requests = listingRequests(byFolder, roots)
			assertEquals("categories", requests.categories?.contentType)
			assertEquals("artists", requests.artists?.contentType)
			assertNull(requests.categories?.musicFolderId)
			assertNull(requests.artists?.musicFolderId)
			assertEquals(PersonalScope.NONE, requests.categories?.personal)
			assertEquals(PersonalScope.NONE, requests.artists?.personal)
		}
	}

	/** A kind may span several roots, so two artist roots are one request. */
	@Test
	fun `roots sharing a content type are one request`() {
		val roots = typed("music" to "artists", "live" to "artists")
		val requests = listingRequests(false, roots)
		assertEquals("artists", requests.artists?.contentType)
		assertNull(requests.categories)
	}

	/**
	 * The group a server lacks is not asked for at all — asking would come
	 * back empty at best, and on a server predating roots as the entire
	 * library, putting the same folders in both groups.
	 */
	@Test
	fun `a server without categories contributes nothing to that group`() {
		assertNull(listingRequests(false, typed("music" to "artists")).categories)
	}

	@Test
	fun `a categories-only server contributes nothing to the artists group`() {
		val requests = listingRequests(false, typed("movies" to "categories"))
		assertEquals("categories", requests.categories?.contentType)
		assertNull(requests.artists)
	}

	/**
	 * A kind this build has never heard of contributes nothing: the merged
	 * list has exactly two groups and nowhere meaningful to put it.
	 */
	@Test
	fun `an unknown content type contributes nothing`() {
		val requests = listingRequests(false, typed("stuff" to "podcasts"))
		assertNull(requests.categories)
		assertNull(requests.artists)
	}

	// ── Untyped servers: everything merges into the artists group ───────

	/**
	 * The replacement for the per-folder chips: a folder-mode server with
	 * several untyped roots is asked once with nothing narrowed, which the
	 * server answers with every root's children merged.
	 */
	@Test
	fun `folder mode with several untyped roots is one unnarrowed request`() {
		val requests = listingRequests(true, untyped("Music", "Podcasts"))
		assertNull(requests.categories)
		assertNull(requests.artists?.contentType)
		assertNull(requests.artists?.musicFolderId)
		assertEquals(PersonalScope.NONE, requests.artists?.personal)
	}

	@Test
	fun `folder mode with a single untyped root narrows nothing`() {
		val requests = listingRequests(true, untyped("Music"))
		assertNull(requests.artists?.musicFolderId)
		assertNull(requests.artists?.contentType)
	}

	/**
	 * Today's behaviour for a server that has never heard of roots, pinned: in
	 * ID3 mode it is still sent `contentType=artists`, which is exactly what
	 * this app sent before folder browsing existed and which it ignores.
	 */
	@Test
	fun `a legacy server in id3 mode is still sent contentType artists`() {
		assertEquals("artists", listingRequests(false, emptyList()).artists?.contentType)
		assertEquals(
			"artists",
			listingRequests(false, untyped("Music", "Podcasts")).artists?.contentType,
		)
	}

	// ── The uploads listing ─────────────────────────────────────────────
	//
	// A fact about the account, not about the roots: the server keeps its
	// uploads root out of getMusicFolders entirely, so it is its own request
	// rather than a half of listingRequests.

	/**
	 * `personal=true` and nothing else. Sending `contentType=uploads` would
	 * narrow the *shared* library to a kind of root no server has, and answer
	 * with an empty list rather than an error.
	 */
	@Test
	fun `uploads is asked as personal and narrows nothing else`() {
		val request = uploadsRequest(isAdmin = false)
		assertEquals(PersonalScope.MINE, request.personal)
		assertEquals("true", request.personalParam)
		assertNull(request.contentType)
		assertNull(request.musicFolderId)
	}

	/**
	 * An admin gets everybody's, because an admin is the only account that can
	 * promote one into the library — without this a non-admin's upload is
	 * visible to its owner and to nobody who can act on it.
	 */
	@Test
	fun `an admin asks for every account's uploads`() {
		val request = uploadsRequest(isAdmin = true)
		assertEquals(PersonalScope.ALL, request.personal)
		assertEquals("*", request.personalParam)
		assertNull(request.contentType)
		assertNull(request.musicFolderId)
	}

	/** The shared library must go on sending exactly what it sent before. */
	@Test
	fun `the shared library sends no personal parameter`() {
		val requests = listingRequests(false, typed("music" to "artists"))
		assertEquals(PersonalScope.NONE, requests.artists?.personal)
		assertNull(requests.artists?.personalParam)
	}
}
