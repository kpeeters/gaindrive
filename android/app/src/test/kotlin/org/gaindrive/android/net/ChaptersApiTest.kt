package org.gaindrive.android.net

import com.jakewharton.retrofit2.converter.kotlinx.serialization.asConverterFactory
import kotlinx.coroutines.test.runTest
import okhttp3.MediaType.Companion.toMediaType
import okhttp3.OkHttpClient
import okhttp3.mockwebserver.MockResponse
import okhttp3.mockwebserver.MockWebServer
import org.junit.After
import org.junit.Assert.assertEquals
import org.junit.Assert.assertTrue
import org.junit.Before
import org.junit.Test
import retrofit2.Retrofit

/**
 * One parse test per chapter shape, against the JSON the server actually
 * emits. The bodies below were taken from `chapter_array_json` and its three
 * callers in `src/gaindrive.cc`, so a server-side shape change should break
 * these before it breaks a screen.
 */
class ChaptersApiTest {

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
	fun tearDown() = server.shutdown()

	private fun respond(body: String) {
		server.enqueue(
			MockResponse().setHeader("Content-Type", "application/json").setBody(body)
		)
	}

	@Test
	fun `getChapters parses a sidecar list`() = runTest {
		respond(
			"""{"subsonic-response":{"status":"ok","chapters":{
			     "id":"41","source":"sidecar","writable":true,"chapter":[
			       {"index":1,"start":0.0,"duration":214,"name":"Shine On"},
			       {"index":2,"start":214.5,"duration":300,"name":"Welcome to the Machine"}]}}}"""
		)
		val found = api.getChapters("41").requireOk().chapters!!
		assertEquals("sidecar", found.source)
		assertTrue(found.writable)
		assertEquals(2, found.chapter.size)
		assertEquals(1, found.chapter[0].index)
		// Milliseconds survive the round trip: the server sends them precisely
		// so that reading a list and writing it back is a fixed point.
		assertEquals(214.5, found.chapter[1].start, 0.0001)
		assertEquals("Welcome to the Machine", found.chapter[1].name)
	}

	@Test
	fun `getChapters reports a container list as read from the file`() = runTest {
		respond(
			"""{"subsonic-response":{"status":"ok","chapters":{
			     "id":"7","source":"container","writable":false,"chapter":[
			       {"index":1,"start":0.0,"duration":90,"name":"Opening"}]}}}"""
		)
		val found = api.getChapters("7").requireOk().chapters!!
		assertEquals("container", found.source)
		assertEquals(false, found.writable)
	}

	@Test
	fun `getChapters tolerates a bare marker and a zero duration`() = runTest {
		// Both are ordinary rather than errors: the server reports an empty name
		// exactly as the file holds it, and gives 0 for a span that is not
		// positive - a marker past the end, or two on one timestamp.
		respond(
			"""{"subsonic-response":{"status":"ok","chapters":{
			     "id":"7","source":"sidecar","writable":true,"chapter":[
			       {"index":1,"start":10.0,"duration":0,"name":""}]}}}"""
		)
		val chapter = api.getChapters("7").requireOk().chapters!!.chapter.single()
		assertEquals("", chapter.name)
		assertEquals(0, chapter.duration)
	}

	@Test
	fun `getChapters reports an empty tombstone as a sidecar with no markers`() = runTest {
		// The only way to say "this one has no chapters" about a rip whose
		// container disagrees, so it must not parse as "no answer".
		respond(
			"""{"subsonic-response":{"status":"ok","chapters":{
			     "id":"7","source":"sidecar","writable":true}}}"""
		)
		val found = api.getChapters("7").requireOk().chapters!!
		assertEquals("sidecar", found.source)
		assertTrue(found.chapter.isEmpty())
	}

	@Test
	fun `getAlbumChapters parses one entry per chaptered item`() = runTest {
		respond(
			"""{"subsonic-response":{"status":"ok","albumChapters":{"id":"12","song":[
			     {"id":"41","title":"Live at Pompeii","chapter":[
			       {"index":1,"start":0.0,"duration":214,"name":"Echoes"}]},
			     {"id":"42","title":"The Encore","chapter":[
			       {"index":1,"start":0.0,"duration":100,"name":"Careful With That Axe"}]}]}}}"""
		)
		val found = api.getAlbumChapters("12").requireOk().albumChapters!!
		assertEquals(listOf("41", "42"), found.song.map { it.id })
		assertEquals("Live at Pompeii", found.song[0].title)
		assertEquals("Echoes", found.song[0].chapter.single().name)
	}

	@Test
	fun `search3 parses chapter matches`() = runTest {
		respond(
			"""{"subsonic-response":{"status":"ok","searchResult3":{
			     "artist":[],"album":[],"song":[],
			     "chapter":[{"songId":"41","parent":"12","index":3,"start":640.25,
			       "name":"Echoes","track":"Live at Pompeii","album":"Pompeii",
			       "artist":"Pink Floyd"}]}}}"""
		)
		val hit = api.search3("echoes", 0, 0, 0, 20).requireOk().searchResult3!!.chapter.single()
		assertEquals("41", hit.songId)
		assertEquals("12", hit.parent)
		assertEquals(3, hit.index)
		assertEquals(640.25, hit.start, 0.0001)
		assertEquals("Pink Floyd", hit.artist)
	}

	@Test
	fun `a search with no chapter key parses as no matches`() = runTest {
		// What every server answers when chapterCount was not sent, and what an
		// older one answers always. It must not be an error.
		respond(
			"""{"subsonic-response":{"status":"ok","searchResult3":{
			     "artist":[],"album":[],"song":[]}}}"""
		)
		val found = api.search3("echoes", 0, 0, 20, 0).requireOk().searchResult3!!
		assertTrue(found.chapter.isEmpty())
	}

	@Test
	fun `starred2 shares the search payload and never carries chapters`() = runTest {
		// The one hazard of that sharing: a reader finding a chapters field on
		// the starred model should find it reliably empty.
		respond(
			"""{"subsonic-response":{"status":"ok","starred2":{
			     "artist":[],"album":[],"song":[]}}}"""
		)
		assertTrue(api.getStarred2().requireOk().starred2!!.chapter.isEmpty())
	}
}
