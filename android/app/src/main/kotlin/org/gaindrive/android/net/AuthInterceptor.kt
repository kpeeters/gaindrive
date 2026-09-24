package org.gaindrive.android.net

import okhttp3.HttpUrl
import okhttp3.Interceptor
import okhttp3.Response
import java.security.MessageDigest
import java.security.SecureRandom

/**
 * Adds the Subsonic authentication and protocol parameters to every request,
 * so no call site has to think about them. One instance per server - there is
 * no global "current credentials", and credentials therefore cannot leak from
 * one server's request into another's by construction.
 *
 * Uses the token scheme (`u`/`t`/`s`) rather than sending the password as `p`,
 * because request URLs reach logcat and Coil's cache keys.
 */
class AuthInterceptor(
	private val username: String,
	password: String,
	/** Injectable so tests are deterministic; production generates one. */
	private val salt: String = newSalt(),
) : Interceptor {

	private val token: String = md5Hex(password + salt)

	override fun intercept(chain: Interceptor.Chain): Response {
		val url = applyTo(chain.request().url.newBuilder()).build()
		return chain.proceed(chain.request().newBuilder().url(url).build())
	}

	/**
	 * Also used by [SubsonicClient.url] for the URLs that are fetched outside
	 * Retrofit - cover art and streams. Sharing this method is what keeps the
	 * hand-built URLs and the intercepted ones from drifting apart.
	 */
	fun applyTo(builder: HttpUrl.Builder): HttpUrl.Builder = builder
		.addQueryParameter("u", username)
		.addQueryParameter("t", token)
		.addQueryParameter("s", salt)
		.addQueryParameter("v", PROTOCOL_VERSION)
		.addQueryParameter("c", CLIENT_NAME)
		.addQueryParameter("f", "json")

	companion object {
		const val PROTOCOL_VERSION = "1.16.1"
		const val CLIENT_NAME = "gaindrive-android"

		/**
		 * Generated once per client instance, not per request, so that a
		 * single screen's worth of cover-art URLs agree with each other.
		 *
		 * It is not what makes Coil's disk cache work, and never was: a client
		 * instance lives in an in-memory map, so this rotates on every app
		 * start and every stored entry keyed on a URL became unreachable with
		 * it. `ArtKeys` is what fixes that, by keying on the request with this
		 * taken back out.
		 */
		fun newSalt(): String {
			val bytes = ByteArray(8)
			SecureRandom().nextBytes(bytes)
			return bytes.toHex()
		}

		fun md5Hex(input: String): String =
			MessageDigest.getInstance("MD5").digest(input.toByteArray(Charsets.UTF_8)).toHex()

		private fun ByteArray.toHex(): String =
			joinToString("") { "%02x".format(it) }
	}
}
