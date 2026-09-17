package org.gaindrive.android.data.cache

import coil3.request.CachePolicy
import coil3.request.ImageRequest

/**
 * The caching every cover-art load wants, in one place so the composables and
 * the notification's bitmap loader cannot drift apart.
 *
 * Two things, both of which the defaults get wrong for this app:
 *
 * * The keys come from [ArtKeys] rather than the URL, so an entry written
 *   before the last app start is still found. See [ArtKeys] for why the URL
 *   itself is unusable.
 * * Offline, the network is not consulted at all. `getCoverArt` answers with
 *   `Cache-Control: no-cache` and an ETag, deliberately, so that art replaced
 *   server-side is picked up despite keeping its URL. Offline that
 *   revalidation cannot succeed, and letting it be attempted would trade a
 *   perfectly good stored copy for a placeholder and a connect timeout per
 *   image.
 */
fun ImageRequest.Builder.artCaching(url: String, online: Boolean): ImageRequest.Builder {
	// Null when the URL will not parse, which leaves Coil's own default in
	// place rather than filing the picture under nothing.
	ArtKeys.cacheKey(url)?.let {
		memoryCacheKey(it)
		diskCacheKey(it)
	}
	if (!online) networkCachePolicy(CachePolicy.DISABLED)
	return this
}
