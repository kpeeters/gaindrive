package org.gaindrive.android.net

import com.jakewharton.retrofit2.converter.kotlinx.serialization.asConverterFactory
import kotlinx.coroutines.test.runTest
import okhttp3.MediaType.Companion.toMediaType
import okhttp3.OkHttpClient
import okhttp3.mockwebserver.MockResponse
import okhttp3.mockwebserver.MockWebServer
import org.junit.After
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Assert.fail
import org.junit.Before
import org.junit.Test
import retrofit2.Retrofit

/**
 * Browsing the personal uploads area, and moving something out of it.
 *
 * The parameter tests matter more than they look: `personal` and `contentType`
 * are both optional and both silently ignored by a server that has never heard
 * of them, so getting one wrong produces a *plausible* listing — the whole
 * shared library under the Uploads chip — rather than an error anybody would
 * notice.
 */
class UploadsApiTest {

	private lateinit var server: MockWebServer
	private lateinit var api: SubsonicApi

	private val json = SubsonicJson

	@Before
	fun setUp() {
		server = MockWebServer()
		server.start()
		api = Retrofit.Builder()
			.baseUrl(server.url("/"))
			.client(OkHttpClient())
			.addConverterFactory(json.asConverterFactory("application/json".toMediaType()))
			.build()
			.create(SubsonicApi::class.java)
	}

	@After
	fun tearDown() {
		server.shutdown()
	}

	private fun respond(body: String) {
		server.enqueue(
			MockResponse()
				.setHeader("Content-Type", "application/json")
				.setBody(body)
		)
	}

	private val emptyArtists =
		"""{"subsonic-response":{"status":"ok","version":"1.16.1",
		   "artists":{"index":[]}}}"""

	private val emptyIndexes =
		"""{"subsonic-response":{"status":"ok","version":"1.16.1",
		   "indexes":{"index":[]}}}"""

	@Test
	fun `getArtists sends personal when browsing uploads`() = runTest {
		respond(emptyArtists)
		api.getArtists("true", null, null).requireOk()

		val path = server.takeRequest().path.orEmpty()
		assertTrue(path.contains("personal=true"))
		assertFalse("uploads is not a contentType", path.contains("contentType"))
	}

	/**
	 * The case that would be invisible if it broke: every ordinary listing must
	 * carry exactly what it carried before uploads existed.
	 */
	@Test
	fun `getArtists sends no personal parameter for the shared library`() = runTest {
		respond(emptyArtists)
		api.getArtists(null, "categories", null).requireOk()

		val path = server.takeRequest().path.orEmpty()
		assertFalse(path.contains("personal"))
		assertTrue(path.contains("contentType=categories"))
	}

	@Test
	fun `getIndexes sends personal too`() = runTest {
		respond(emptyIndexes)
		api.getIndexes(null, null, "true").requireOk()

		val path = server.takeRequest().path.orEmpty()
		assertTrue(path.contains("personal=true"))
		assertFalse(path.contains("musicFolderId"))
	}

	@Test
	fun `getIndexes for a folder chip is unchanged`() = runTest {
		respond(emptyIndexes)
		api.getIndexes("2", null, null).requireOk()

		val path = server.takeRequest().path.orEmpty()
		assertTrue(path.contains("musicFolderId=2"))
		assertFalse(path.contains("personal"))
	}

	private val okEnvelope = """{"subsonic-response":{"status":"ok","version":"1.16.1"}}"""

	/**
	 * The whole destination, always. Both halves are required — they were
	 * briefly optional and each default was a guess that filed things wrongly,
	 * the root one unable to reach a `categories` root at all — which is why the
	 * declaration takes them non-null rather than leaving it to a call site to
	 * remember.
	 */
	@Test
	fun `moveAlbum sends the id, the root and the folder`() = runTest {
		respond(okEnvelope)
		api.moveAlbum("412", "3", "Documentaries").requireOk()

		val path = server.takeRequest().path.orEmpty()
		assertTrue(path.contains("id=412"))
		assertTrue(path.contains("musicFolderId=3"))
		assertTrue(path.contains("folder=Documentaries"))
	}

	/**
	 * Not an admin. The screen only draws the action for one, but the server
	 * decides — and naming a destination root is the half of `moveAlbum` that
	 * requires admin, since it is what puts something into the shared library.
	 */
	@Test
	fun `moveAlbum without admin is error 50`() = runTest {
		respond(
			"""{"subsonic-response":{"status":"failed","version":"1.16.1",
			   "error":{"code":50,"message":"Moving into the shared library requires admin role."}}}"""
		)
		try {
			api.moveAlbum("412", "3", "Pink Floyd").requireOk()
			fail("expected SubsonicException")
		} catch (e: SubsonicException) {
			assertEquals(SubsonicException.NOT_AUTHORISED, e.code)
			assertEquals("Moving into the shared library requires admin role.", e.message)
		}
	}

	/** An id that is not a browsable library root — including the uploads root. */
	@Test
	fun `an unusable destination root is error 70`() = runTest {
		respond(
			"""{"subsonic-response":{"status":"failed","version":"1.16.1",
			   "error":{"code":70,"message":"No such library folder."}}}"""
		)
		try {
			api.moveAlbum("412", "99", "Pink Floyd").requireOk()
			fail("expected SubsonicException")
		} catch (e: SubsonicException) {
			assertEquals(SubsonicException.NOT_FOUND, e.code)
		}
	}

	@Test
	fun `deleteUpload sends the album id`() = runTest {
		respond(okEnvelope)
		api.deleteUpload("412").requireOk()

		assertTrue(server.takeRequest().path.orEmpty().contains("id=412"))
	}

	/**
	 * The refusal that is the security boundary: an id naming anything but the
	 * caller's own upload. One message covers "not an upload" and "not yours",
	 * deliberately — the difference is only useful to somebody probing ids — so
	 * the client must show what the server said rather than inventing a reason.
	 */
	@Test
	fun `deleting something that is not your upload is refused with a message`() = runTest {
		respond(
			"""{"subsonic-response":{"status":"failed","version":"1.16.1",
			   "error":{"code":0,"message":"Item is not in your uploads."}}}"""
		)
		try {
			api.deleteUpload("412").requireOk()
			fail("expected SubsonicException")
		} catch (e: SubsonicException) {
			assertEquals("Item is not in your uploads.", e.message)
		}
	}

	@Test
	fun `deleting an id that resolves to nothing is error 70`() = runTest {
		respond(
			"""{"subsonic-response":{"status":"failed","version":"1.16.1",
			   "error":{"code":70,"message":"Item not found."}}}"""
		)
		try {
			api.deleteUpload("99999").requireOk()
			fail("expected SubsonicException")
		} catch (e: SubsonicException) {
			assertEquals(SubsonicException.NOT_FOUND, e.code)
		}
	}

	/** No upload role at all. Distinct from the two above, and from error 70. */
	@Test
	fun `deleting without upload rights is error 50`() = runTest {
		respond(
			"""{"subsonic-response":{"status":"failed","version":"1.16.1",
			   "error":{"code":50,"message":"User is not authorized for the given operation."}}}"""
		)
		try {
			api.deleteUpload("412").requireOk()
			fail("expected SubsonicException")
		} catch (e: SubsonicException) {
			assertTrue(e.isForbidden)
		}
	}

	/**
	 * A destination that is already occupied. The server answers error 0 with
	 * its own wording, which the panel shows as-is rather than translating.
	 *
	 * This replaced a test for "item is not in a personal library folder",
	 * which `promoteAlbum` used to raise from a five-component source-path
	 * check. `moveAlbum` has no such check — with it an admin could not move a
	 * library album at all, which was the point of merging the two endpoints —
	 * so that message no longer exists on the server to assert against.
	 */
	@Test
	fun `an occupied destination is refused with a message`() = runTest {
		respond(
			"""{"subsonic-response":{"status":"failed","version":"1.16.1",
			   "error":{"code":0,"message":"Something with that name is already in that folder."}}}"""
		)
		try {
			api.moveAlbum("7", "3", "Pink Floyd").requireOk()
			fail("expected SubsonicException")
		} catch (e: SubsonicException) {
			assertEquals("Something with that name is already in that folder.", e.message)
		}
	}
}
