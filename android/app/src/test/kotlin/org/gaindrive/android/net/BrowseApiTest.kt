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

	// The production parser, not a copy of its settings: a copy proves the DTOs
	// match some configuration, which is exactly the gap a legacy server's
	// unquoted ids slipped through.
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
	fun `getArtists parses index buckets`() = runTest {
		respond(
			"""{"subsonic-response":{"status":"ok","artists":{"lastModified":0,
			   "ignoredArticles":"The","index":[
			     {"name":"S","artist":[
			       {"id":"12","name":"Steely Dan","albumCount":9,"coverArt":"12"}]},
			     {"name":"#","artist":[
			       {"id":"31","name":"10cc","albumCount":2,"coverArt":"31"}]}]}}}"""
		)
		val index = api.getArtists(null, null).requireOk().artists!!.index
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
		assertEquals("ok", api.star("501", null, null).requireOk().status)
	}

	// ── Legacy servers ──────────────────────────────────────────────────
	//
	// gaindrive quotes every id, but Subsonic's own XSD types several of them
	// as integers and older servers emit them unquoted. One such field used to
	// fail the whole response, which surfaced as "Expected quotation mark but
	// had '1'" the moment a legacy server was enabled — and the artist list
	// went with it, because getMusicFolders is fetched first to learn which
	// root kinds the server has.

	@Test
	fun `getMusicFolders accepts an unquoted id`() = runTest {
		respond(
			"""{"subsonic-response":{"status":"ok","version":"1.16.1",
			   "musicFolders":{"musicFolder":[{"id":1,"name":"Music"}]}}}"""
		)
		val folders = api.getMusicFolders().requireOk().musicFolders!!.musicFolder
		assertEquals("1", folders[0].id)
		// Absent, not empty: a server with no concept of root kinds must stay
		// distinguishable from one that named them.
		assertNull(folders[0].contentType)
	}

	@Test
	fun `browse entries accept unquoted ids throughout`() = runTest {
		respond(
			"""{"subsonic-response":{"status":"ok","album":{"id":77,"parent":12,
			   "artistId":12,"name":"The Royal Scam","songCount":1,"song":[
			     {"id":501,"parent":77,"albumId":77,"title":"Kid Charlemagne",
			      "coverArt":77,"duration":279}]}}}"""
		)
		val album = api.getAlbum("77").requireOk().album!!
		assertEquals("77", album.id)
		assertEquals("12", album.artistId)
		assertEquals("501", album.song[0].id)
		assertEquals("77", album.song[0].coverArt)
	}

	/** The mirror image, for a server that quotes what the DTO calls a number. */
	@Test
	fun `numbers survive being quoted`() = runTest {
		respond(
			"""{"subsonic-response":{"status":"ok","album":{"id":"77","name":"X",
			   "songCount":"1","duration":"2340","song":[
			     {"id":"501","title":"Peg","track":"3","bitRate":"961"}]}}}"""
		)
		val album = api.getAlbum("77").requireOk().album!!
		assertEquals(1, album.songCount)
		assertEquals(2340, album.duration)
		assertEquals(3, album.song[0].track)
		assertEquals(961, album.song[0].bitRate)
	}

	// ── Video ───────────────────────────────────────────────────────────

	@Test
	fun `a video entry carries its video fields`() = runTest {
		respond(
			"""{"subsonic-response":{"status":"ok","album":{"id":"9",
			   "name":"The Third Man (1949)","songCount":1,"duration":6240,
			   "song":[{"id":"900","title":"The Third Man","isDir":false,
			     "type":"video","isVideo":true,"nativeSeek":true,
			     "suffix":"mp4","contentType":"video/mp4","duration":6240,
			     "originalWidth":1920,"originalHeight":1080}]}}}"""
		)
		val entry = api.getAlbum("9").requireOk().album!!.song[0]
		assertTrue(entry.isVideo)
		assertTrue(entry.nativeSeek)
		assertEquals(1920, entry.originalWidth)
	}

	/**
	 * Only four server queries select the codec columns `nativeSeek` is derived
	 * from, so a video reached through search or a playlist arrives without it.
	 * False is the safe reading — such a video is played over HLS, which works
	 * for everything — and the parser must produce that rather than a default
	 * of true.
	 */
	@Test
	fun `a video without nativeSeek defaults to false`() = runTest {
		respond(
			"""{"subsonic-response":{"status":"ok","searchResult3":{"song":[
			   {"id":"900","title":"The Third Man","isVideo":true}]}}}"""
		)
		val song = api.search3("third", 0, 0, 20).requireOk().searchResult3!!.song[0]
		assertTrue(song.isVideo)
		assertFalse(song.nativeSeek)
	}

	@Test
	fun `getVideoInfo parses the caption list`() = runTest {
		respond(
			"""{"subsonic-response":{"status":"ok","videoInfo":{"id":"900",
			   "captions":[{"id":"2","name":"English"},{"id":"3","name":"Dutch"}],
			   "audioTrack":[{"id":"1","name":"aac","languageCode":"eng"}]}}}"""
		)
		val info = api.getVideoInfo("900").requireOk().videoInfo!!
		assertEquals(2, info.captions.size)
		assertEquals("English", info.captions[0].name)
		assertEquals("2", info.captions[0].id)
	}

	/** A DVD's subtitles are bitmaps, so the server filters them all out. */
	@Test
	fun `getVideoInfo with no captions parses to an empty list`() = runTest {
		respond("""{"subsonic-response":{"status":"ok","videoInfo":{"id":"900"}}}""")
		assertTrue(api.getVideoInfo("900").requireOk().videoInfo!!.captions.isEmpty())
	}

	// ── Folder browsing ─────────────────────────────────────────────────

	@Test
	fun `getIndexes parses index buckets without an album count`() = runTest {
		respond(
			"""{"subsonic-response":{"status":"ok","indexes":{"lastModified":0,
			   "ignoredArticles":"The El La","index":[
			     {"name":"S","artist":[
			       {"id":"12","name":"Steely Dan","coverArt":"12"}]}]}}}"""
		)
		val index = api.getIndexes(null, null, null).requireOk().indexes!!.index
		assertEquals(1, index.size)
		assertEquals("S", index[0].name)
		assertEquals("Steely Dan", index[0].artist[0].name)
		// The folder listing does not count albums; the mapper must not invent
		// one and the row must not print a zero.
		assertEquals(0, index[0].artist[0].albumCount)
	}

	@Test
	fun `getMusicDirectory tells child directories from songs`() = runTest {
		respond(
			"""{"subsonic-response":{"status":"ok","directory":{"id":"12",
			   "name":"Steely Dan","parent":"1","coverArt":"12","child":[
			     {"id":"77","parent":"12","isDir":true,"title":"Aja",
			      "artist":"Steely Dan","album":"Aja","coverArt":"77","year":1977},
			     {"id":"501","parent":"12","albumId":"12","isDir":false,
			      "title":"Loose Ends","duration":211}]}}}"""
		)
		val dir = api.getMusicDirectory("12").requireOk().directory!!
		assertEquals("Steely Dan", dir.name)
		assertEquals("1", dir.parent)
		val (dirs, songs) = dir.child.partition { it.isDir }
		assertEquals(1, dirs.size)
		assertEquals("Aja", dirs[0].title)
		assertEquals(1977, dirs[0].year)
		assertEquals(1, songs.size)
		assertEquals("Loose Ends", songs[0].title)
	}

	/** Both are omitted rather than sent as null at the top of a root. */
	@Test
	fun `getMusicDirectory omits parent and coverArt at a root`() = runTest {
		respond(
			"""{"subsonic-response":{"status":"ok","directory":{"id":"1",
			   "name":"music","child":[]}}}"""
		)
		val dir = api.getMusicDirectory("1").requireOk().directory!!
		assertNull(dir.parent)
		assertNull(dir.coverArt)
		assertTrue(dir.child.isEmpty())
	}

	/** The same leniency the artist list needed; see the section above. */
	@Test
	fun `getMusicDirectory tolerates unquoted ids`() = runTest {
		respond(
			"""{"subsonic-response":{"status":"ok","directory":{"id":12,
			   "name":"Steely Dan","parent":1,"child":[
			     {"id":77,"parent":12,"isDir":true,"title":"Aja"}]}}}"""
		)
		val dir = api.getMusicDirectory("12").requireOk().directory!!
		assertEquals("12", dir.id)
		assertEquals("1", dir.parent)
		assertEquals("77", dir.child[0].id)
		assertTrue(dir.child[0].isDir)
	}

	@Test
	fun `search2 parses folder-shaped albums`() = runTest {
		respond(
			"""{"subsonic-response":{"status":"ok","searchResult2":{
			   "album":[{"id":"77","parent":"12","title":"Aja","artist":"Steely Dan"}],
			   "song":[{"id":"501","parent":"77","title":"Peg"}]}}}"""
		)
		val found = api.search2("aja", 5, 5, 5).requireOk().searchResult2!!
		assertEquals("77", found.album[0].id)
		// Folder-shaped: the title arrives as `title`, and the parent stands in
		// for artistId.
		assertEquals("Aja", found.album[0].title)
		assertEquals("12", found.album[0].parent)
		assertEquals("Peg", found.song[0].title)
	}
}
