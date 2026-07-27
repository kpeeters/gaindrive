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
import org.junit.Assert.fail
import org.junit.Before
import org.junit.Test
import retrofit2.Retrofit

/**
 * The API layer is where bugs are both likely and invisible: a mis-parsed
 * envelope looks like working code and fails only on real data.
 */
class SubsonicApiTest {

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

	@Test
	fun `ok envelope unwraps`() = runTest {
		respond("""{"subsonic-response":{"status":"ok","version":"1.16.1"}}""")
		val body = api.ping().requireOk()
		assertEquals("ok", body.status)
	}

	@Test
	fun `wrong credentials become a typed error`() = runTest {
		respond(
			"""{"subsonic-response":{"status":"failed","version":"1.16.1",
			   "error":{"code":40,"message":"Wrong username or password."}}}"""
		)
		try {
			api.ping().requireOk()
			fail("expected SubsonicException")
		} catch (e: SubsonicException) {
			assertEquals(SubsonicException.WRONG_CREDENTIALS, e.code)
			assertTrue(e.isAuthFailure)
			assertEquals("Wrong username or password.", e.message)
		}
	}

	@Test
	fun `not authorised is distinguishable from bad credentials`() = runTest {
		respond(
			"""{"subsonic-response":{"status":"failed",
			   "error":{"code":50,"message":"Not authorized."}}}"""
		)
		try {
			api.ping().requireOk()
			fail("expected SubsonicException")
		} catch (e: SubsonicException) {
			assertTrue(e.isForbidden)
			assertTrue(!e.isAuthFailure)
		}
	}

	/** A failed status with no error object must not produce a confusing null. */
	@Test
	fun `failed status without an error object still throws`() = runTest {
		respond("""{"subsonic-response":{"status":"failed"}}""")
		try {
			api.ping().requireOk()
			fail("expected SubsonicException")
		} catch (e: SubsonicException) {
			assertEquals(0, e.code)
			assertTrue(e.message.isNotBlank())
		}
	}

	/** Servers gain fields over time; an unknown key must never fail a response. */
	@Test
	fun `unknown fields are ignored`() = runTest {
		respond(
			"""{"subsonic-response":{"status":"ok","version":"1.16.1",
			   "openSubsonic":true,"somethingNew":{"nested":[1,2,3]}}}"""
		)
		assertEquals("ok", api.ping().requireOk().status)
	}

	@Test
	fun `getUser maps roles`() = runTest {
		respond(
			"""{"subsonic-response":{"status":"ok","user":{"username":"admin",
			   "adminRole":true,"uploadRole":false,"maxBitRate":320}}}"""
		)
		val user = api.getUser("admin").requireOk().user!!
		assertEquals("admin", user.username)
		assertTrue(user.adminRole)
		assertTrue(!user.uploadRole)
		assertEquals(320, user.maxBitRate)
	}

	/** Absent optional fields must fall back rather than throw. */
	@Test
	fun `getUser tolerates a minimal user object`() = runTest {
		respond("""{"subsonic-response":{"status":"ok","user":{"username":"bob"}}}""")
		val user = api.getUser("bob").requireOk().user!!
		assertEquals("bob", user.username)
		assertTrue(!user.adminRole)
		assertEquals(0, user.maxBitRate)
		assertNull(user.email)
	}
}
