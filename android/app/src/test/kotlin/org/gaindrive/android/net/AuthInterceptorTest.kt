package org.gaindrive.android.net

import okhttp3.OkHttpClient
import okhttp3.Request
import okhttp3.mockwebserver.MockResponse
import okhttp3.mockwebserver.MockWebServer
import org.junit.After
import org.junit.Assert.assertEquals
import org.junit.Assert.assertNotEquals
import org.junit.Assert.assertNull
import org.junit.Before
import org.junit.Test

class AuthInterceptorTest {

	private lateinit var server: MockWebServer

	@Before
	fun setUp() {
		server = MockWebServer()
		server.start()
	}

	@After
	fun tearDown() {
		server.shutdown()
	}

	private fun call(interceptor: AuthInterceptor): okhttp3.HttpUrl {
		server.enqueue(MockResponse().setBody("{}"))
		val client = OkHttpClient.Builder().addInterceptor(interceptor).build()
		client.newCall(Request.Builder().url(server.url("/rest/ping.view")).build())
			.execute().close()
		return server.takeRequest().requestUrl!!
	}

	@Test
	fun `adds the token scheme and never the password`() {
		val url = call(AuthInterceptor("admin", "secret", salt = "abcdef"))

		assertEquals("admin", url.queryParameter("u"))
		assertEquals("abcdef", url.queryParameter("s"))
		assertEquals("1.16.1", url.queryParameter("v"))
		assertEquals("gaindrive-android", url.queryParameter("c"))
		assertEquals("json", url.queryParameter("f"))

		// The plaintext password must never appear in a URL: these reach
		// logcat and Coil's cache keys.
		assertNull(url.queryParameter("p"))
	}

	/**
	 * The server computes md5(password + salt) and compares case-insensitively
	 * (see validate_auth in src/mediastore.cc), so this value has to match
	 * exactly what it derives.
	 */
	@Test
	fun `token matches the server's md5 of password plus salt`() {
		val url = call(AuthInterceptor("admin", "sesame", salt = "c19b2d"))
		val expected = AuthInterceptor.md5Hex("sesame" + "c19b2d")
		assertEquals(expected, url.queryParameter("t"))
		assertEquals(expected, expected.lowercase())
	}

	/**
	 * Salt stability is what lets Coil's disk cache hit: a per-request salt
	 * would make every cover-art URL unique and re-download on every scroll.
	 */
	@Test
	fun `salt is stable across requests from one interceptor`() {
		val interceptor = AuthInterceptor("admin", "secret")
		val first = call(interceptor)
		val second = call(interceptor)

		assertEquals(first.queryParameter("s"), second.queryParameter("s"))
		assertEquals(first.queryParameter("t"), second.queryParameter("t"))
	}

	@Test
	fun `separate interceptors get separate salts`() {
		val a = call(AuthInterceptor("admin", "secret"))
		val b = call(AuthInterceptor("admin", "secret"))
		assertNotEquals(a.queryParameter("s"), b.queryParameter("s"))
	}

	@Test
	fun `md5 is the known digest`() {
		// RFC 1321 test vector, so a broken digest is caught here rather than
		// as a mysterious 40 from the server.
		assertEquals("900150983cd24fb0d6963f7d28e17f72", AuthInterceptor.md5Hex("abc"))
	}
}
