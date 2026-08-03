package org.gaindrive.android.net

import com.jakewharton.retrofit2.converter.kotlinx.serialization.asConverterFactory
import kotlinx.serialization.json.Json
import okhttp3.HttpUrl.Companion.toHttpUrl
import okhttp3.MediaType.Companion.toMediaType
import org.junit.Assert.assertEquals
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Test
import retrofit2.Retrofit

/**
 * The shape of the URLs video playback builds.
 *
 * `StreamUrls.forVideo` chooses between these two endpoints and adds nothing
 * else, so what is worth pinning down is the part that would be silently wrong:
 * the `.view` suffix `hls.m3u8` must not have, and the two audio parameters
 * that must never appear on a video request.
 *
 *  - `format` is validated against the audio target table, so a container name
 *    is rejected outright and an audio one gets the soundtrack alone.
 *  - `maxBitRate` forces the server's re-encode tier, demoting a file that
 *    could have been served straight off disk.
 */
class VideoUrlsTest {

	// Never called: url() builds strings. Retrofit's create() issues no
	// request, so this needs no server and no dispatcher.
	private val api: SubsonicApi = Retrofit.Builder()
		.baseUrl("https://music.example.org/")
		.addConverterFactory(Json.asConverterFactory("application/json".toMediaType()))
		.build()
		.create(SubsonicApi::class.java)

	private val client = SubsonicClient(
		serverId = null,
		baseUrl = "https://music.example.org",
		username = "admin",
		api = api,
		auth = AuthInterceptor("admin", "secret", salt = "abcdef"),
	)

	@Test
	fun `a seekable video streams progressively with no audio parameters`() {
		val url = client.url("stream", mapOf("id" to "900")).toHttpUrl()

		assertTrue(url.encodedPath.endsWith("/rest/stream.view"))
		assertEquals("900", url.queryParameter("id"))
		assertNull(url.queryParameter("format"))
		assertNull(url.queryParameter("maxBitRate"))
	}

	/**
	 * `hls.m3u8` is the one endpoint not spelled `<name>.view`. Appending the
	 * suffix would 404 every re-encoded video, and that file extension is also
	 * what lets ExoPlayer recognise the playlist without being told.
	 */
	@Test
	fun `the HLS playlist keeps its own extension`() {
		val url = client.url("hls.m3u8", mapOf("id" to "900"), suffix = "").toHttpUrl()

		assertEquals("/rest/hls.m3u8", url.encodedPath)
		assertEquals("900", url.queryParameter("id"))
	}

	/**
	 * The playlist copies the caller's credentials onto every segment URL, so
	 * the request that fetches it has to carry them in the first place.
	 */
	@Test
	fun `the HLS playlist request is authenticated`() {
		val url = client.url("hls.m3u8", mapOf("id" to "900"), suffix = "").toHttpUrl()

		assertEquals("admin", url.queryParameter("u"))
		assertEquals(AuthInterceptor.md5Hex("secret" + "abcdef"), url.queryParameter("t"))
		assertEquals("abcdef", url.queryParameter("s"))
		assertNull(url.queryParameter("p"))
	}

	@Test
	fun `captions are fetched per stream index from the same server`() {
		val url = client.url(
			"getCaptions",
			mapOf("id" to "900", "captionId" to "2"),
		).toHttpUrl()

		assertTrue(url.encodedPath.endsWith("/rest/getCaptions.view"))
		assertEquals("900", url.queryParameter("id"))
		assertEquals("2", url.queryParameter("captionId"))
	}
}
