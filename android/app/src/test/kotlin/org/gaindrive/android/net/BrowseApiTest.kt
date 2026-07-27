package org.gaindrive.android.net

import com.jakewharton.retrofit2.converter.kotlinx.serialization.asConverterFactory
import kotlinx.coroutines.test.runTest
import kotlinx.serialization.json.Json
import okhttp3.MediaType.Companion.toMediaType
import okhttp3.OkHttpClient
import okhttp3.mockwebserver.MockResponse
import okhttp3.mockwebserver.MockWebServer
import org.junit.After
import org.junit.Assert.assertEquals
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Before
import org.junit.Test
import retrofit2.Retrofit

/**
 * One parse test per browse endpoint shape, against the JSON the server
 * actually emits. The bodies below were taken from `src/gaindrive.cc`, so a
 * server-side shape change should break these before it breaks a screen.
 */
class BrowseApiTest {

	private lateinit var server: MockWebServer
	private lateinit var api: SubsonicApi

	private val json = Json {
		ignoreUnknownKeys = true
		coerceInputValues = true
	}

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
	fun `getArtists parses index buckets`() = runTest {
		respond(
			"""{"subsonic-response":{"status":"ok","artists":{"lastModified":0,
			   "ignoredArticles":"The","index":[
			     {"name":"S","artist":[
			       {"id":"12","name":"Steely Dan","albumCount":9,"coverArt":"12"}]},
			     {"name":"#","artist":[
			       {"id":"31","name":"10cc","albumCount":2,"coverArt":"31"}]}]}}}"""
		)
		val index = api.getArtists().requireOk().artists!!.index
		assertEquals(2, index.size)
		assertEquals("S", index[0].name)
		assertEquals("Steely Dan", index[0].artist[0].name)
		assertEquals("12", index[0].artist[0].id)
		assertEquals(9, index[0].artist[0].albumCount)
	}

	@Test
	fun `getArtist parses albums with artistId`() = runTest {
		respond(
			"""{"subsonic-response":{"status":"ok","artist":{"id":"12",
			   "name":"Steely Dan","albumCount":1,"album":[
			     {"id":"77","parent":"12","artistId":"12","name":"The Royal Scam",
			      "title":"The Royal Scam","artist":"Steely Dan","songCount":9,
			      "duration":2340,"created":"1976-05-01T00:00:00Z","coverArt":"77",
			      "year":1976,"genre":"Rock"}]}}}"""
		)
		val artist = api.getArtist("12").requireOk().artist!!
		assertEquals(1, artist.album.size)
		val album = artist.album[0]
		assertEquals("77", album.id)
		assertEquals("12", album.artistId)
		assertEquals(1976, album.year)
	}

	@Test
	fun `getAlbum parses songs`() = runTest {
		respond(
			"""{"subsonic-response":{"status":"ok","album":{"id":"77","parent":"12",
			   "artistId":"12","name":"The Royal Scam","artist":"Steely Dan",
			   "songCount":1,"duration":279,"coverArt":"77","song":[
			     {"id":"501","parent":"77","albumId":"77","isDir":false,
			      "type":"music","isVideo":false,"title":"Kid Charlemagne",
			      "artist":"Steely Dan","album":"The Royal Scam","track":1,
			      "discNumber":1,"year":1976,"genre":"Rock","size":6710886,
			      "contentType":"audio/flac","suffix":"flac","duration":279,
			      "bitRate":961,"coverArt":"77"}]}}}"""
		)
		val album = api.getAlbum("77").requireOk().album!!
		assertEquals(1, album.song.size)
		val song = album.song[0]
		assertEquals("501", song.id)
		assertEquals("77", song.albumId)
		assertEquals(279, song.duration)
		assertEquals(6710886L, song.size)
	}

	/**
	 * `starred` became an ISO 8601 instant rather than a boolean when the
	 * server was made conformant. Parsing it as a string is the whole point.
	 */
	@Test
	fun `starred is a timestamp`() = runTest {
		respond(
			"""{"subsonic-response":{"status":"ok","album":{"id":"77","name":"X",
			   "starred":"2026-07-27T18:04:11Z","song":[
			     {"id":"501","title":"Y","starred":"2026-07-01T09:00:00Z"}]}}}"""
		)
		val album = api.getAlbum("77").requireOk().album!!
		assertEquals("2026-07-27T18:04:11Z", album.starred)
		assertEquals("2026-07-01T09:00:00Z", album.song[0].starred)
	}

	/** Absence means not starred; it must not become a false-y string. */
	@Test
	fun `absent starred stays null`() = runTest {
		respond("""{"subsonic-response":{"status":"ok","album":{"id":"77","name":"X"}}}""")
		assertNull(api.getAlbum("77").requireOk().album!!.starred)
	}

	@Test
	fun `song with only mandatory fields parses`() = runTest {
		respond(
			"""{"subsonic-response":{"status":"ok","album":{"id":"77","name":"X",
			   "song":[{"id":"9","title":"Untitled"}]}}}"""
		)
		val song = api.getAlbum("77").requireOk().album!!.song[0]
		assertEquals("9", song.id)
		assertEquals("Untitled", song.title)
		assertNull(song.artist)
		assertNull(song.track)
		assertEquals(0, song.duration)
	}

	@Test
	fun `search3 parses all three categories`() = runTest {
		respond(
			"""{"subsonic-response":{"status":"ok","searchResult3":{
			   "artist":[{"id":"12","name":"Steely Dan"}],
			   "album":[{"id":"77","name":"Aja"}],
			   "song":[{"id":"501","title":"Peg"}]}}}"""
		)
		val result = api.search3("steely", 20, 20, 50).requireOk().searchResult3!!
		assertEquals(1, result.artist.size)
		assertEquals(1, result.album.size)
		assertEquals(1, result.song.size)
	}

	/** Empty categories are omitted entirely rather than sent as []. */
	@Test
	fun `search3 with no matches parses`() = runTest {
		respond("""{"subsonic-response":{"status":"ok","searchResult3":{}}}""")
		val result = api.search3("zzz", 20, 20, 50).requireOk().searchResult3!!
		assertTrue(result.artist.isEmpty() && result.album.isEmpty() && result.song.isEmpty())
	}

	@Test
	fun `getPlaylist parses entries`() = runTest {
		respond(
			"""{"subsonic-response":{"status":"ok","playlist":{"id":"3",
			   "name":"Late night","owner":"admin","public":false,"songCount":1,
			   "duration":279,"created":"2026-01-02T03:04:05Z",
			   "changed":"2026-01-02T03:04:05Z",
			   "entry":[{"id":"501","title":"Kid Charlemagne"}]}}}"""
		)
		val playlist = api.getPlaylist("3").requireOk().playlist!!
		assertEquals("Late night", playlist.name)
		assertEquals(1, playlist.entry.size)
		assertTrue(!playlist.public)
	}

	@Test
	fun `getRecentSongs carries lastPlayed`() = runTest {
		respond(
			"""{"subsonic-response":{"status":"ok","recentSongs":{"song":[
			   {"id":"501","title":"Peg","lastPlayed":"2026-07-27T17:00:00Z"}]}}}"""
		)
		val songs = api.getRecentSongs(50).requireOk().recentSongs!!.song
		assertEquals("2026-07-27T17:00:00Z", songs[0].lastPlayed)
	}

	@Test
	fun `getAlbumInfo2 parses notes and links`() = runTest {
		respond(
			"""{"subsonic-response":{"status":"ok","albumInfo2":{
			   "notes":"The fifth studio album.",
			   "wikiUrl":"https://en.wikipedia.org/wiki/The_Royal_Scam",
			   "allMusicUrl":"https://allmusic.com/album/x"}}}"""
		)
		val info = api.getAlbumInfo2("77").requireOk().albumInfo2!!
		assertEquals("The fifth studio album.", info.notes)
		assertTrue(info.wikiUrl!!.startsWith("https://"))
	}

	@Test
	fun `star returns a bare ok`() = runTest {
		respond("""{"subsonic-response":{"status":"ok"}}""")
		assertEquals("ok", api.star(songId = "501").requireOk().status)
	}
}
