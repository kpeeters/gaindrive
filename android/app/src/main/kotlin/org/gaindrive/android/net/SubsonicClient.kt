package org.gaindrive.android.net

import okhttp3.HttpUrl.Companion.toHttpUrlOrNull
import org.gaindrive.android.data.model.ServerId

/**
 * One server's API surface. Holds the Retrofit-generated [SubsonicApi] and the
 * knowledge needed to build URLs that are fetched outside Retrofit — cover art
 * for Coil, and streams for ExoPlayer and the cast bridge.
 */
class SubsonicClient(
	val serverId: ServerId?,
	val baseUrl: String,
	val username: String,
	private val api: SubsonicApi,
	private val auth: AuthInterceptor,
) : SubsonicApi by api {

	/**
	 * Builds a URL for an endpoint fetched by something other than Retrofit.
	 * Carries the same auth parameters the interceptor would have added — this
	 * is the one place they are constructed by hand.
	 *
	 * [suffix] exists for `hls.m3u8`, the one endpoint in the API that is not
	 * spelled `<name>.view`. Passing an empty string leaves the endpoint name
	 * alone; ExoPlayer also infers HLS from that trailing `.m3u8`, which is a
	 * happy accident rather than something to rely on.
	 */
	fun url(
		endpoint: String,
		params: Map<String, String> = emptyMap(),
		suffix: String = ".view",
	): String {
		val base = "$baseUrl/rest/$endpoint$suffix".toHttpUrlOrNull()
			?: error("Server URL is not a valid HTTP URL: $baseUrl")
		val builder = base.newBuilder()
		params.forEach { (k, v) -> builder.addQueryParameter(k, v) }
		return auth.applyTo(builder).build().toString()
	}
}

/**
 * Outcome of a connection test. "The server said no" and "there was no server"
 * need different fixes, so the UI must be able to tell them apart.
 */
sealed interface ConnectionTest {
	data object Reachable : ConnectionTest
	data class Rejected(val message: String) : ConnectionTest
	data class Unreachable(val message: String) : ConnectionTest

	/**
	 * The server is there and answered `ping`, but the call that would have
	 * proved the credentials took too long to wait for. Not a failure — a large
	 * library can legitimately be slow — but not the reassurance the button
	 * exists to give either, so it says so rather than claiming success.
	 */
	data object Unverified : ConnectionTest
}
