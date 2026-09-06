package org.gaindrive.android.data.browse

import com.jakewharton.retrofit2.converter.kotlinx.serialization.asConverterFactory
import kotlinx.coroutines.test.runTest
import okhttp3.MediaType.Companion.toMediaType
import okhttp3.OkHttpClient
import okhttp3.mockwebserver.Dispatcher
import okhttp3.mockwebserver.MockResponse
import okhttp3.mockwebserver.MockWebServer
import okhttp3.mockwebserver.RecordedRequest
import org.gaindrive.android.data.model.ItemRef
import org.gaindrive.android.data.model.ServerId
import org.gaindrive.android.net.SubsonicApi
import org.gaindrive.android.net.SubsonicJson
import org.junit.After
import org.junit.Assert.assertEquals
import org.junit.Assert.assertNull
import org.junit.Before
import org.junit.Test
import retrofit2.Retrofit

/**
 * Reading the hierarchy through the folder endpoints.
 *
 * Driven against a real Retrofit and the production parser, the same harness
 * `BrowseApiTest` uses — which is why [BrowseSource] takes [SubsonicApi] rather
 * than the client wrapping it: there is no mocking library in this project, and
 * none is needed.
 */
class FolderSourceTest {

	private lateinit var server: MockWebServer
	private lateinit var api: SubsonicApi

	private val serverId = ServerId("srv-a")

	@Before
	fun setUp() {
		server = MockWebServer()
		server.start()
		api = Retrofit.Builder()
			.baseUrl(server.url("/"))
			.client(OkHttpClient())
			.addConverterFactory(SubsonicJson.asConverterFactory("application/json".toMediaType()))
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

	/**
	 * Answer by the `id` a request asks for, rather than in the order responses
	 * were queued.
	 *
	 * MockWebServer's default dispatcher is FIFO, so a caller that fetches
	 * concurrently — [FolderSource.albumDetail] does, one request per disc
	 * folder — can be handed the body belonging to the other request. That is a
	 * coin flip, not a test. Setting a dispatcher replaces the queue, but
	 * [setUp] builds a fresh server per test, so the sequential tests above keep
	 * their [respond].
	 */
	private fun respondById(bodies: Map<String, String>) {
		server.dispatcher = object : Dispatcher() {
			override fun dispatch(request: RecordedRequest): MockResponse {
				val id = request.requestUrl?.queryParameter("id")
				val body = id?.let { bodies[it] }
					?: return MockResponse().setResponseCode(404)
				return MockResponse()
					.setHeader("Content-Type", "application/json").setBody(body)
			}
		}
	}

	private fun ref(id: String) = ItemRef(serverId, id)

	@Test
	fun `an artist's subdirectories become its albums`() = runTest {
		respond(
			"""{"subsonic-response":{"status":"ok","directory":{"id":"12",
			   "name":"Steely Dan","parent":"1","child":[
			     {"id":"77","parent":"12","isDir":true,"title":"Aja","year":1977},
			     {"id":"78","parent":"12","isDir":true,"title":"Gaucho"},
			     {"id":"501","parent":"12","isDir":false,"title":"a stray track"}]}}}"""
		)
		val albums = FolderSource.albums(api, ref("12"))
		assertEquals(listOf("Aja", "Gaucho"), albums.map { it.title })
		assertEquals("12", albums[0].artistRef?.id)
		assertEquals(1977, albums[0].year)
		// The folder listing does not count tracks, so the row omits it.
		assertEquals(0, albums[0].songCount)
	}

	@Test
	fun `a directory reached as an artist counts only its subdirectories`() = runTest {
		respond(
			"""{"subsonic-response":{"status":"ok","directory":{"id":"12",
			   "name":"Steely Dan","child":[
			     {"id":"77","isDir":true,"title":"Aja"},
			     {"id":"501","isDir":false,"title":"a stray track"}]}}}"""
		)
		val artist = FolderSource.artist(api, ref("12"))!!
		assertEquals("Steely Dan", artist.name)
		assertEquals(1, artist.albumCount)
	}

	@Test
	fun `an album directory of songs becomes a flat detail`() = runTest {
		respond(
			"""{"subsonic-response":{"status":"ok","directory":{"id":"77",
			   "name":"Aja","parent":"12","coverArt":"77","child":[
			     {"id":"501","parent":"77","isDir":false,"title":"Black Cow",
			      "artist":"Steely Dan","duration":314},
			     {"id":"502","parent":"77","isDir":false,"title":"Aja",
			      "artist":"Steely Dan","duration":479}]}}}"""
		)
		val detail = FolderSource.albumDetail(api, ref("77"))!!
		assertEquals("Aja", detail.album.title)
		assertEquals("Steely Dan", detail.album.artistName)
		assertEquals(2, detail.album.songCount)
		assertEquals(793, detail.album.duration)
		assertEquals(listOf("Black Cow", "Aja"), detail.songs.map { it.title })
	}

	@Test
	fun `an album directory of subfolders is flattened into discs`() = runTest {
		// The two disc folders are fetched concurrently, so the mock has to
		// answer by id — see respondById.
		respondById(
			mapOf(
				// The album folder: two discs and no tracks of its own.
				"77" to """{"subsonic-response":{"status":"ok","directory":{"id":"77",
				   "name":"The Wall","parent":"12","child":[
				     {"id":"90","parent":"77","isDir":true,"title":"CD1"},
				     {"id":"91","parent":"77","isDir":true,"title":"CD2"}]}}}""",
				"90" to """{"subsonic-response":{"status":"ok","directory":{"id":"90",
				   "name":"CD1","parent":"77","child":[
				     {"id":"501","parent":"90","isDir":false,"title":"In the Flesh?"}]}}}""",
				"91" to """{"subsonic-response":{"status":"ok","directory":{"id":"91",
				   "name":"CD2","parent":"77","child":[
				     {"id":"601","parent":"91","isDir":false,"title":"Hey You"}]}}}""",
			)
		)

		val detail = FolderSource.albumDetail(api, ref("77"))!!
		assertEquals(listOf("In the Flesh?", "Hey You"), detail.songs.map { it.title })
		assertEquals(listOf(1, 2), detail.songs.map { it.discNumber })
		// Two discs are not two series.
		detail.songs.forEach { assertNull(it.season) }
		assertEquals("The Wall", detail.album.title)
	}

	/**
	 * The regression guard for pin coverage: a track's album must be the album
	 * folder, never the disc folder it physically sits in and never the ID3
	 * album id it also carries. `LibraryDao.songsOfAlbum` keys on this, and it is
	 * what an album pin protects.
	 */
	@Test
	fun `songs carry the album directory's ref, not their own parent`() = runTest {
		respond(
			"""{"subsonic-response":{"status":"ok","directory":{"id":"77",
			   "name":"The Wall","child":[
			     {"id":"90","parent":"77","isDir":true,"title":"CD1"}]}}}"""
		)
		respond(
			"""{"subsonic-response":{"status":"ok","directory":{"id":"90",
			   "name":"CD1","parent":"77","child":[
			     {"id":"501","parent":"90","albumId":"5000","isDir":false,
			      "title":"In the Flesh?"}]}}}"""
		)
		val detail = FolderSource.albumDetail(api, ref("77"))!!
		assertEquals("77", detail.songs[0].albumRef?.id)
	}

	/** A bonus-material folder beside the tracks is not a second disc. */
	@Test
	fun `subdirectories are ignored when the folder holds tracks of its own`() = runTest {
		respond(
			"""{"subsonic-response":{"status":"ok","directory":{"id":"77",
			   "name":"Aja","child":[
			     {"id":"90","parent":"77","isDir":true,"title":"Scans"},
			     {"id":"501","parent":"77","isDir":false,"title":"Black Cow"}]}}}"""
		)
		val detail = FolderSource.albumDetail(api, ref("77"))!!
		assertEquals(listOf("Black Cow"), detail.songs.map { it.title })
		// One request only — nothing went looking inside "Scans".
		assertEquals(1, server.requestCount)
	}

	/**
	 * A reference from the tag hierarchy — the mirror written before the switch
	 * was flipped, or a starred album — still opens.
	 */
	@Test
	fun `a directory that is not found falls back to getAlbum`() = runTest {
		respond(
			"""{"subsonic-response":{"status":"failed",
			   "error":{"code":70,"message":"Directory not found."}}}"""
		)
		respond(
			"""{"subsonic-response":{"status":"ok","album":{"id":"5000",
			   "name":"Aja","artist":"Steely Dan","song":[
			     {"id":"501","title":"Black Cow"}]}}}"""
		)
		val detail = FolderSource.albumDetail(api, ref("5000"))!!
		assertEquals("Aja", detail.album.title)
		assertEquals(listOf("Black Cow"), detail.songs.map { it.title })
	}

	/** The same, for an id that resolved but named nothing playable. */
	@Test
	fun `an empty directory falls back to getAlbum`() = runTest {
		respond(
			"""{"subsonic-response":{"status":"ok","directory":{"id":"5000",
			   "name":"not an album","child":[]}}}"""
		)
		respond(
			"""{"subsonic-response":{"status":"ok","album":{"id":"5000",
			   "name":"Aja","song":[{"id":"501","title":"Black Cow"}]}}}"""
		)
		val detail = FolderSource.albumDetail(api, ref("5000"))!!
		assertEquals("Aja", detail.album.title)
	}

	@Test
	fun `indexes are read from getIndexes`() = runTest {
		respond(
			"""{"subsonic-response":{"status":"ok","indexes":{"index":[
			   {"name":"S","artist":[{"id":"12","name":"Steely Dan"}]},
			   {"name":"P","artist":[]}]}}}"""
		)
		val indexes = FolderSource.indexes(api, serverId, RootRequest(musicFolderId = "1"))
		assertEquals(listOf("S", "P"), indexes.map { it.label })
		assertEquals("Steely Dan", indexes[0].artists[0].name)
		assertEquals("musicFolderId=1", server.takeRequest().path?.substringAfter('?'))
	}

	@Test
	fun `search asks search2 so its ids match the rest of the mode`() = runTest {
		respond(
			"""{"subsonic-response":{"status":"ok","searchResult2":{
			   "album":[{"id":"77","parent":"12","title":"Aja"}]}}}"""
		)
		val found = FolderSource.search(api, serverId, "aja", 5, 5, 5, 0)
		assertEquals("77", found.albums[0].ref.id)
		assertEquals("/rest/search2.view", server.takeRequest().path?.substringBefore('?'))
	}
}
