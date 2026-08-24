package org.gaindrive.android.data.browse

import org.gaindrive.android.data.model.LibraryMode
import org.gaindrive.android.data.model.MusicRoot
import org.junit.Assert.assertEquals
import org.junit.Assert.assertNull
import org.junit.Test

/**
 * What the chip row offers, and what a chosen chip sends.
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

	// ── Content types win, in either mode ───────────────────────────────

	/**
	 * The guarantee that makes the setting safe on a gaindrive server: it names
	 * kinds of root, so the chips are those kinds whichever way the hierarchy is
	 * being read, and flipping the switch changes only the endpoints.
	 */
	@Test
	fun `a server naming content types offers them in both modes`() {
		val roots = typed("music" to "artists", "movies" to "categories")
		val expected = listOf(LibraryMode("artists"), LibraryMode("categories"))
		assertEquals(expected, chipsFor(browseByFolder = false, roots = roots))
		assertEquals(expected, chipsFor(browseByFolder = true, roots = roots))
	}

	/** A kind may span several roots, so two artist roots are one chip. */
	@Test
	fun `roots sharing a content type collapse to one chip`() {
		val roots = typed("music" to "artists", "live" to "artists")
		assertEquals(listOf(LibraryMode("artists")), chipsFor(false, roots))
	}

	@Test
	fun `a content type chip is sent as contentType`() {
		val roots = typed("music" to "artists", "movies" to "categories")
		val request = rootRequest(false, roots, LibraryMode("categories"))
		assertEquals("categories", request?.contentType)
		assertNull(request?.musicFolderId)
	}

	// ── Music folders, folder mode only ─────────────────────────────────

	@Test
	fun `folder mode with several untyped roots offers one chip each`() {
		val roots = untyped("Music", "Podcasts")
		assertEquals(
			listOf(LibraryMode.folder("Music"), LibraryMode.folder("Podcasts")),
			chipsFor(browseByFolder = true, roots = roots),
		)
	}

	@Test
	fun `a folder chip is sent as that server's own musicFolderId`() {
		val roots = untyped("Music", "Podcasts")
		val request = rootRequest(true, roots, LibraryMode.folder("Podcasts"))
		assertEquals("2", request?.musicFolderId)
		assertNull(request?.contentType)
	}

	/**
	 * The threshold that keeps the common case looking untouched: one library
	 * needs no chip to choose between, and a row holding a single chip would be
	 * a visible change for someone who gained nothing by it.
	 */
	@Test
	fun `folder mode with a single root offers artists and narrows nothing`() {
		val roots = untyped("Music")
		assertEquals(listOf(LibraryMode.ARTISTS), chipsFor(true, roots))
		val request = rootRequest(true, roots, LibraryMode.ARTISTS)
		assertNull(request?.musicFolderId)
		assertNull(request?.contentType)
	}

	@Test
	fun `id3 mode never offers folder chips`() {
		val roots = untyped("Music", "Podcasts")
		assertEquals(listOf(LibraryMode.ARTISTS), chipsFor(browseByFolder = false, roots = roots))
	}

	/**
	 * Today's behaviour for a server that has never heard of roots, pinned: it
	 * is still sent `contentType=artists`, which it ignores.
	 */
	@Test
	fun `a legacy server in id3 mode is still sent contentType artists`() {
		val request = rootRequest(false, emptyList(), LibraryMode.ARTISTS)
		assertEquals("artists", request?.contentType)
	}

	/** A chip only some servers offer must not become a request to the rest. */
	@Test
	fun `a chip this server does not offer yields no request`() {
		val roots = typed("music" to "artists")
		assertNull(rootRequest(false, roots, LibraryMode("categories")))
		assertNull(rootRequest(false, roots, LibraryMode.folder("Podcasts")))
	}

	/** A folder renamed on the server no longer answers for the stored chip. */
	@Test
	fun `a folder chip naming no root here yields no request`() {
		assertNull(rootRequest(true, untyped("Music", "Podcasts"), LibraryMode.folder("Films")))
	}

	// ── The uploads slice ───────────────────────────────────────────────
	//
	// A fact about the account, not about the roots: the server keeps its
	// uploads root out of getMusicFolders entirely, so nothing in `roots` can
	// ever produce this chip and nothing in `roots` can ever suppress it.

	@Test
	fun `uploads is appended to content type chips`() {
		val roots = typed("music" to "artists", "movies" to "categories")
		assertEquals(
			listOf(LibraryMode("artists"), LibraryMode("categories"), LibraryMode.UPLOADS),
			chipsFor(browseByFolder = false, roots = roots, canUpload = true),
		)
	}

	@Test
	fun `uploads is appended to folder chips`() {
		val roots = untyped("Music", "Podcasts")
		assertEquals(
			listOf(LibraryMode.folder("Music"), LibraryMode.folder("Podcasts"), LibraryMode.UPLOADS),
			chipsFor(browseByFolder = true, roots = roots, canUpload = true),
		)
	}

	/**
	 * The one chip allowed to turn a would-be single-chip row into a real one.
	 * The threshold that suppresses a lone folder chip exists because that row
	 * offered nothing; this one offers somewhere else to be.
	 */
	@Test
	fun `uploads gives a single library server a chip row`() {
		assertEquals(
			listOf(LibraryMode.ARTISTS, LibraryMode.UPLOADS),
			chipsFor(browseByFolder = true, roots = untyped("Music"), canUpload = true),
		)
	}

	@Test
	fun `an account that cannot upload is offered nothing extra`() {
		val roots = typed("music" to "artists")
		assertEquals(
			listOf(LibraryMode("artists")),
			chipsFor(browseByFolder = false, roots = roots, canUpload = false),
		)
		assertNull(rootRequest(false, roots, LibraryMode.UPLOADS, canUpload = false))
	}

	/**
	 * `personal=true` and nothing else. Sending `contentType=uploads` would
	 * narrow the *shared* library to a kind of root no server has, and answer
	 * with an empty list rather than an error.
	 */
	@Test
	fun `the uploads chip is sent as personal and narrows nothing else`() {
		val roots = typed("music" to "artists")
		val request = rootRequest(false, roots, LibraryMode.UPLOADS, canUpload = true)
		assertEquals(true, request?.personal)
		assertEquals("true", request?.personalParam)
		assertNull(request?.contentType)
		assertNull(request?.musicFolderId)
	}

	/** Every other slice must go on sending exactly what it sent before. */
	@Test
	fun `an ordinary slice sends no personal parameter`() {
		val roots = typed("music" to "artists")
		val request = rootRequest(false, roots, LibraryMode("artists"), canUpload = true)
		assertEquals(false, request?.personal)
		assertNull(request?.personalParam)
	}

	/** Uploads sorts last whatever it is spelled next to. */
	@Test
	fun `uploads is ranked after content types and folders`() {
		val merged = mergeChips(
			listOf(
				listOf(LibraryMode.UPLOADS, LibraryMode.folder("Music")),
				listOf(LibraryMode("videos"), LibraryMode("artists")),
			)
		)
		assertEquals(
			listOf(
				LibraryMode("artists"),
				LibraryMode("videos"),
				LibraryMode.folder("Music"),
				LibraryMode.UPLOADS,
			),
			merged,
		)
	}

	// ── Unioning across servers ─────────────────────────────────────────

	@Test
	fun `chips union by name, content types before folders`() {
		val merged = mergeChips(
			listOf(
				listOf(LibraryMode.folder("Podcasts"), LibraryMode.folder("Music")),
				listOf(LibraryMode("categories"), LibraryMode("artists")),
			)
		)
		assertEquals(
			listOf(
				LibraryMode("artists"),
				LibraryMode("categories"),
				LibraryMode.folder("Music"),
				LibraryMode.folder("Podcasts"),
			),
			merged,
		)
	}

	/**
	 * Two servers with a folder of the same name are one chip — which is the
	 * whole reason a folder is keyed on its name and not on its id, those being
	 * per-server and freely colliding.
	 */
	@Test
	fun `the same folder name on two servers is one chip`() {
		val merged = mergeChips(
			listOf(listOf(LibraryMode.folder("Music")), listOf(LibraryMode.folder("music")))
		)
		assertEquals(1, merged.size)
		// The first contributor's spelling survives, registry order deciding as
		// it does everywhere else.
		assertEquals("Music", merged[0].label)
	}

	@Test
	fun `a folder cannot be mistaken for the content type of the same name`() {
		assertEquals("folder:artists", LibraryMode.folder("artists").id)
		assertEquals("artists", LibraryMode.folder("artists").label)
		assertNull(LibraryMode.ARTISTS.folderName)
	}
}
