package org.gaindrive.android.data.cache

import okhttp3.HttpUrl.Companion.toHttpUrlOrNull
import org.gaindrive.android.data.model.ItemRef
import org.gaindrive.android.net.SubsonicClient
import java.security.MessageDigest

/**
 * Stable identities for a cover-art request, derived from the URL rather than
 * carried beside it.
 *
 * A `getCoverArt` URL is *not* usable as a cache key, which is what this
 * exists to fix. `AuthInterceptor` generates a fresh salt per client instance
 * and clients live in an in-memory map, so `t` and `s` change on every app
 * start. Coil keys its disk cache on the model string and OkHttp keys on the
 * URL, so before this every entry written by the previous session was
 * unreachable and every cover was fetched again, including offline, where
 * fetching is exactly what cannot happen.
 *
 * Derived rather than passed alongside because art round-trips through Media3
 * as a bare `MediaMetadata.artworkUri` (see `MediaItems.toMediaItem`), so the
 * URL is the only thing that survives to the notification and the queue. A
 * second value would have to be smuggled through as an extra, and it would be
 * fully determined by the first anyway.
 */
object ArtKeys {

	/**
	 * Per session (`t`, `s`), per protocol (`u`, `v`, `c`, `f`) or per retry
	 * (`_r`). None of them name the picture, and all of them change under it.
	 */
	private val VOLATILE = setOf("u", "t", "s", "v", "c", "f", "_r")

	/**
	 * What Coil files the bytes under, in memory and on disk.
	 *
	 * `size` is kept: the server serves a different ladder rung per size, so
	 * two sizes really are two pictures.
	 */
	fun cacheKey(url: String): String? = canonical(url, keepSize = true)

	/**
	 * What identifies the picture itself, whatever size was asked for.
	 *
	 * Used to look up a pinned file, of which there is one per picture, so the
	 * size has to come out.
	 */
	fun lookupKey(url: String): String? = canonical(url, keepSize = false)

	/**
	 * The same key for the side that holds a ref rather than a URL.
	 *
	 * Deliberately goes through [SubsonicClient.url] rather than assembling a
	 * shorter string of its own: the index and the lookup have to agree
	 * character for character, and the only way to guarantee that is for both
	 * to be the same function of the same URL builder.
	 */
	fun lookupKey(client: SubsonicClient, coverArtId: String): String? =
		lookupKey(client.url("getCoverArt", mapOf("id" to coverArtId)))

	/**
	 * The name a pinned file is stored under.
	 *
	 * Hashed from the ref rather than the URL so that editing a server's
	 * address does not strand every file it downloaded. Hex of a digest
	 * because a cover art id is a server-side path fragment and may contain
	 * anything a filename may not.
	 */
	fun fileName(ref: ItemRef): String =
		MessageDigest.getInstance("SHA-256")
			.digest(ref.encode().toByteArray(Charsets.UTF_8))
			.joinToString("") { "%02x".format(it) }

	/**
	 * The request with everything volatile removed and the rest in a fixed
	 * order, so two spellings of the same request agree.
	 *
	 * Null for anything that is not a parsable HTTP URL, which callers treat
	 * as "no stable identity" and fall back to Coil's own default.
	 */
	private fun canonical(url: String, keepSize: Boolean): String? {
		val parsed = url.toHttpUrlOrNull() ?: return null
		val kept = (0 until parsed.querySize)
			.map { parsed.queryParameterName(it) to parsed.queryParameterValue(it) }
			.filter { (name, _) -> name !in VOLATILE }
			.filter { (name, _) -> keepSize || name != "size" }
			.sortedBy { it.first }
			.joinToString("&") { (name, value) -> "$name=$value" }
		// The path stays in: a server can host several libraries behind one
		// host, and an id is only unique within its own server.
		return parsed.newBuilder().query(null).build().toString() + "?" + kept
	}
}
