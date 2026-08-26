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
	 * No destination at all. This is the compatibility shape — the server falls
	 * back to the first `artists` root under its own artist name — and it must
	 * keep sending *nothing*, or a server that predates the parameters would see
	 * an empty `folder` where it expects none.
	 */
	@Test
	fun `promoteAlbum with no destination sends only the id`() = runTest {
		respond(okEnvelope)
		api.promoteAlbum("412", null, null).requireOk()

		val path = server.takeRequest().path.orEmpty()
		assertTrue(path.contains("id=412"))
		assertFalse(path.contains("musicFolderId"))
		assertFalse(path.contains("folder"))
	}

	@Test
	fun `promoteAlbum sends a chosen root and folder`() = runTest {
		respond(okEnvelope)
		api.promoteAlbum("412", "3", "Documentaries").requireOk()

		val path = server.takeRequest().path.orEmpty()
		assertTrue(path.contains("musicFolderId=3"))
		assertTrue(path.contains("folder=Documentaries"))
	}

	/** A root with no folder: keep the batch's own artist name, but put it there. */
	@Test
	fun `promoteAlbum can send a root without a folder`() = runTest {
		respond(okEnvelope)
		api.promoteAlbum("412", "3", null).requireOk()

		val path = server.takeRequest().path.orEmpty()
		assertTrue(path.contains("musicFolderId=3"))
		assertFalse(path.contains("folder"))
	}

	/** Not an admin. The screen only draws the action for one, but the server decides. */
	@Test
	fun `promoteAlbum without admin is error 50`() = runTest {
		respond(
			"""{"subsonic-response":{"status":"failed","version":"1.16.1",
			   "error":{"code":50,"message":"Promote requires admin role."}}}"""
		)
		try {
			api.promoteAlbum("412", null, null).requireOk()
			fail("expected SubsonicException")
		} catch (e: SubsonicException) {
			assertEquals(SubsonicException.NOT_AUTHORISED, e.code)
			assertEquals("Promote requires admin role.", e.message)
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
			api.promoteAlbum("412", "99", null).requireOk()
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
	 * An album that is not in a personal folder. The server answers error 0 with
	 * its own wording, which the panel shows as-is rather than translating.
	 */
	@Test
	fun `promoting something outside uploads is refused with a message`() = runTest {
		respond(
			"""{"subsonic-response":{"status":"failed","version":"1.16.1",
			   "error":{"code":0,"message":"Item is not in a personal library folder."}}}"""
		)
		try {
			api.promoteAlbum("7", null, null).requireOk()
			fail("expected SubsonicException")
		} catch (e: SubsonicException) {
			assertEquals("Item is not in a personal library folder.", e.message)
		}
	}
}
