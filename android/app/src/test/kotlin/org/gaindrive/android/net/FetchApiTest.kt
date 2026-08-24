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
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Assert.fail
import org.junit.Before
import org.junit.Test
import retrofit2.Retrofit

/**
 * The URL-fetch endpoints.
 *
 * The parameter-omission test is the one that matters: the server reads an
 * absent `artist` as "keep whatever the handler parsed out of the video's
 * title" and a present-but-empty one as an error, so sending `artist=` for an
 * untouched field would refuse every fetch the user did not name by hand.
 */
class FetchApiTest {

	private lateinit var server: MockWebServer
	private lateinit var api: SubsonicApi

	// The production parser, not a copy of its settings — see BrowseApiTest.
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

	private fun okJob(state: String = "queued") =
		"""{"subsonic-response":{"status":"ok","version":"1.16.1","fetchJob":{
		   "id":"abc","batch":"uploads/kasper/abc","handler":"yt-dlp","mode":"audio",
		   "artist":"","album":"","state":"$state"}}}"""

	@Test
	fun `an untouched name is not sent at all`() = runTest {
		respond(okJob())
		api.fetchUrl("https://youtu.be/x", "audio", null, null).requireOk()

		val path = server.takeRequest().path.orEmpty()
		assertTrue("the URL is sent", path.contains("url=https"))
		assertTrue("the mode is sent", path.contains("mode=audio"))
		assertFalse("an absent artist adds no parameter", path.contains("artist"))
		assertFalse("an absent album adds no parameter", path.contains("album"))
	}

	@Test
	fun `typed names are sent`() = runTest {
		respond(okJob())
		api.fetchUrl("https://youtu.be/x", "video", "Pink Floyd", "The Wall").requireOk()

		val path = server.takeRequest().path.orEmpty()
		assertTrue(path.contains("artist=Pink%20Floyd") || path.contains("artist=Pink+Floyd"))
		assertTrue(path.contains("album=The%20Wall") || path.contains("album=The+Wall"))
		assertTrue(path.contains("mode=video"))
	}

	@Test
	fun `a queued job comes back with its id`() = runTest {
		respond(okJob())
		val job = api.fetchUrl("https://youtu.be/x", "audio", null, null).requireOk().fetchJob
		assertEquals("abc", job?.id)
		assertEquals("yt-dlp", job?.handler)
		assertEquals("queued", job?.state)
	}

	@Test
	fun `handlers parse and report what each can do`() = runTest {
		respond(
			"""{"subsonic-response":{"status":"ok","version":"1.16.1","urlHandlers":{
			   "urlHandler":[{"name":"yt-dlp","audio":true,"video":true},
			                 {"name":"audio-only","audio":true,"video":false}]}}}"""
		)
		val handlers = api.getUrlHandlers().requireOk().urlHandlers?.urlHandler.orEmpty()
		assertEquals(2, handlers.size)
		assertTrue(handlers[0].video)
		assertFalse(handlers[1].video)
	}

	/** No handler table configured. Not an error — the feature is just off. */
	@Test
	fun `an empty handler list is not an error`() = runTest {
		respond(
			"""{"subsonic-response":{"status":"ok","version":"1.16.1",
			   "urlHandlers":{"urlHandler":[]}}}"""
		)
		val body = api.getUrlHandlers().requireOk()
		assertTrue(body.urlHandlers?.urlHandler.orEmpty().isEmpty())
	}

	/**
	 * An account without the upload role. The panel treats this as "do not offer
	 * this server", so it must arrive as a typed error rather than a parse
	 * failure.
	 */
	@Test
	fun `no upload role is error 50`() = runTest {
		respond(
			"""{"subsonic-response":{"status":"failed","version":"1.16.1",
			   "error":{"code":50,"message":"User is not authorized for the given operation."}}}"""
		)
		try {
			api.getUrlHandlers().requireOk()
			fail("expected SubsonicException")
		} catch (e: SubsonicException) {
			assertEquals(SubsonicException.NOT_AUTHORISED, e.code)
			assertTrue(e.isForbidden)
		}
	}

	@Test
	fun `a running job carries its progress and detail`() = runTest {
		respond(
			"""{"subsonic-response":{"status":"ok","version":"1.16.1","fetchJobs":{
			   "fetchJob":[{"id":"abc","handler":"yt-dlp","mode":"audio","state":"running",
			                "percent":63,"detail":"[download]  63.0% of 4.20MiB","files":0}]}}}"""
		)
		val jobs = api.getFetchJobs().requireOk().fetchJobs?.fetchJob.orEmpty()
		assertEquals(1, jobs.size)
		assertEquals(63, jobs[0].percent)
		assertEquals("running", jobs[0].state)
		// Absent in the payload, and must not fail the parse.
		assertEquals("", jobs[0].error)
	}

	/** Nothing queued at all: an empty list, not a missing container. */
	@Test
	fun `no jobs parses to an empty list`() = runTest {
		respond(
			"""{"subsonic-response":{"status":"ok","version":"1.16.1",
			   "fetchJobs":{"fetchJob":[]}}}"""
		)
		assertTrue(api.getFetchJobs().requireOk().fetchJobs?.fetchJob.orEmpty().isEmpty())
	}

	/**
	 * A state this build has never heard of must reach the UI as a string, not
	 * take the response down with it — which is why the DTO does not type it as
	 * an enum.
	 */
	@Test
	fun `an unknown state still parses`() = runTest {
		respond(
			"""{"subsonic-response":{"status":"ok","version":"1.16.1","fetchJobs":{
			   "fetchJob":[{"id":"abc","state":"paused"}]}}}"""
		)
		val jobs = api.getFetchJobs().requireOk().fetchJobs?.fetchJob.orEmpty()
		assertEquals("paused", jobs[0].state)
	}

	@Test
	fun `cancelling an unknown job is error 70`() = runTest {
		respond(
			"""{"subsonic-response":{"status":"failed","version":"1.16.1",
			   "error":{"code":70,"message":"Item not found."}}}"""
		)
		try {
			api.cancelFetch("nope").requireOk()
			fail("expected SubsonicException")
		} catch (e: SubsonicException) {
			assertEquals(SubsonicException.NOT_FOUND, e.code)
		}
	}

	@Test
	fun `a server that never heard of the endpoint leaves the container null`() = runTest {
		respond("""{"subsonic-response":{"status":"ok","version":"1.16.1"}}""")
		assertNull(api.getUrlHandlers().requireOk().urlHandlers)
	}
}
