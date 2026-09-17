package org.gaindrive.android.data.cache

import okhttp3.HttpUrl.Companion.toHttpUrl
import org.gaindrive.android.data.model.ItemRef
import org.gaindrive.android.data.model.ServerId
import org.gaindrive.android.net.AuthInterceptor
import org.junit.Assert.assertEquals
import org.junit.Assert.assertNotEquals
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Test

/**
 * The identity a cached picture is filed under.
 *
 * Worth testing on its own because the bug it exists to prevent is invisible
 * until it is expensive: a key that changes between app starts costs nothing
 * online beyond a re-fetch, and offline it is the difference between a
 * downloaded album having artwork and a screen of placeholders.
 */
class ArtKeysTest {

	private val base = "https://music.example.org/rest/getCoverArt.view"

	/** The same URL a `SubsonicClient` would build, for a given salt. */
	private fun url(id: String, size: Int?, salt: String): String {
		val builder = base.toHttpUrl().newBuilder()
		builder.addQueryParameter("id", id)
		size?.let { builder.addQueryParameter("size", it.toString()) }
		return AuthInterceptor("admin", "secret", salt = salt)
			.applyTo(builder).build().toString()
	}

	@Test
	fun `a new session salt does not change the key`() {
		// The regression this class exists for: AuthInterceptor generates a
		// fresh salt per instance, and instances do not outlive the process.
		val first = url("42", size = 144, salt = "aaaaaaaa")
		val second = url("42", size = 144, salt = "bbbbbbbb")

		assertNotEquals(first, second)
		assertEquals(ArtKeys.cacheKey(first), ArtKeys.cacheKey(second))
		assertEquals(ArtKeys.lookupKey(first), ArtKeys.lookupKey(second))
	}

	@Test
	fun `a portrait retry does not change the key`() {
		// ArtistAvatar appends this to force a re-request past a cached 404.
		// The URL has to differ or nothing is asked again; the key must not,
		// or the portrait that finally arrives is filed under a string no
		// later screen will ask for.
		val plain = url("art-9", size = 288, salt = "aaaaaaaa")
		val retried = "$plain&_r=3"

		assertEquals(ArtKeys.cacheKey(plain), ArtKeys.cacheKey(retried))
	}

	@Test
	fun `size separates cache entries but not pinned files`() {
		// The server serves a different ladder rung per size, so two sizes are
		// two pictures to cache. There is only ever one file on disk, though,
		// which is why the lookup drops it.
		val small = url("42", size = 144, salt = "aaaaaaaa")
		val large = url("42", size = 800, salt = "aaaaaaaa")

		assertNotEquals(ArtKeys.cacheKey(small), ArtKeys.cacheKey(large))
		assertEquals(ArtKeys.lookupKey(small), ArtKeys.lookupKey(large))
	}

	@Test
	fun `a URL with no size at all matches one that has it`() {
		// PinnedArt.index builds without a size; the composables build with
		// one. They have to agree or no pinned file is ever found.
		val sized = url("42", size = 800, salt = "aaaaaaaa")
		val bare = url("42", size = null, salt = "cccccccc")

		assertEquals(ArtKeys.lookupKey(bare), ArtKeys.lookupKey(sized))
	}

	@Test
	fun `the same cover id on two servers is two pictures`() {
		val here = url("42", size = 144, salt = "aaaaaaaa")
		val there = here.replace("music.example.org", "other.example.org")

		assertNotEquals(ArtKeys.cacheKey(here), ArtKeys.cacheKey(there))
		assertNotEquals(ArtKeys.lookupKey(here), ArtKeys.lookupKey(there))
	}

	@Test
	fun `something that is not a URL has no key`() {
		assertNull(ArtKeys.cacheKey("not a url"))
		assertNull(ArtKeys.lookupKey(""))
	}

	@Test
	fun `a file name is stable per ref and unique across servers`() {
		val ref = ItemRef(ServerId("a"), "42")
		val elsewhere = ItemRef(ServerId("b"), "42")

		assertEquals(ArtKeys.fileName(ref), ArtKeys.fileName(ref))
		assertNotEquals(ArtKeys.fileName(ref), ArtKeys.fileName(elsewhere))
		// Usable as a filename, whatever the cover art id happened to contain.
		val awkward = ItemRef(ServerId("a"), "Some Artist/Album (2001)/cover.jpg")
		assertTrue(ArtKeys.fileName(awkward).all { it in "0123456789abcdef" })
	}
}
