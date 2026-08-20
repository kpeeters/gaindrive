package org.gaindrive.android.data.cache

import android.content.Context
import android.util.Log
import coil3.SingletonImageLoader
import dagger.hilt.android.qualifiers.ApplicationContext
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.asStateFlow
import kotlinx.coroutines.withContext
import okhttp3.OkHttpClient
import javax.inject.Inject
import javax.inject.Singleton

/**
 * Cover art and artist portraits held on the device, and the one action that
 * throws them away.
 *
 * Unlike [AudioCache] this exists for a single purpose: art that has changed on
 * the server is the one kind of staleness a user can see and cannot otherwise
 * do anything about. A cover art id is a folder id, so a poster replaced
 * server-side keeps its URL; `getCoverArt` sends an `ETag` and `no-cache` for
 * exactly that reason, and a revalidation still beats a stale image — but when
 * something does go wrong, "delete the lot and fetch it again" is the only
 * remedy that needs no diagnosis.
 *
 * **Images live in two places**, and clearing one alone achieves nothing:
 *
 * * Coil's own disk and memory caches, configured in `GainDriveApplication`.
 * * The shared OkHttp cache, because Coil is built on the same client every
 *   Retrofit instance uses (see `AppModule`). Coil re-fetching an image it has
 *   dropped would otherwise be served the old bytes from there.
 *
 * The OkHttp side is cleared by URL rather than wholesale: that cache also
 * holds API responses, and dropping those would make this button quietly mean
 * "re-fetch the library" as well.
 *
 * **Artist portraits are included**, since an artist's cover art id is its
 * folder id and so its URL carries [ART_ENDPOINT] like any other. They are the
 * one case where clearing can be the actual remedy rather than a precaution: a
 * portrait is resolved server-side after the first request, and a 404 saying
 * "there is none" is cacheable for an hour, so a device can be holding a "no"
 * that the server has since changed its mind about. Note that this only drops
 * the stored answers — nothing is re-fetched until something asks again, which
 * for a portrait is `ArtistAvatar` next time it is composed.
 */
@Singleton
class ImageCache @Inject constructor(
	@ApplicationContext private val context: Context,
	private val httpClient: OkHttpClient,
) {

	private val _sizeBytes = MutableStateFlow(0L)

	/**
	 * Coil's disk cache only — the number is a label for a button, not an
	 * accounting of everything [clear] touches. The memory cache is transient
	 * and the art inside the HTTP cache cannot be sized without walking it.
	 */
	val sizeBytes: StateFlow<Long> = _sizeBytes.asStateFlow()

	suspend fun refreshSize() = withContext(Dispatchers.IO) {
		_sizeBytes.value = runCatching {
			SingletonImageLoader.get(context).diskCache?.size ?: 0L
		}.getOrDefault(0L)
	}

	/** Never throws: this is a settings button, and a failure to clear one of
	 *  the two caches must not stop the other. */
	suspend fun clear() = withContext(Dispatchers.IO) {
		val loader = SingletonImageLoader.get(context)
		runCatching { loader.memoryCache?.clear() }
			.onFailure { Log.w(TAG, "memory cache: ${it.message}") }
		runCatching { loader.diskCache?.clear() }
			.onFailure { Log.w(TAG, "disk cache: ${it.message}") }
		runCatching { clearHttpArt() }
			.onFailure { Log.w(TAG, "http cache: ${it.message}") }
		refreshSize()
	}

	/**
	 * Drops the art out of the shared HTTP cache, leaving API responses alone.
	 * `urls()` iterates the cache's journal and its `remove` deletes the entry
	 * it just returned.
	 */
	private fun clearHttpArt() {
		val urls = httpClient.cache?.urls() ?: return
		var removed = 0
		while (urls.hasNext()) {
			if (ART_ENDPOINT in urls.next()) {
				urls.remove()
				removed++
			}
		}
		Log.i(TAG, "dropped $removed cached image response(s)")
	}

	private companion object {
		const val TAG = "ImageCache"

		/**
		 * The only endpoint this app loads an image from — `CoverUrls` builds
		 * nothing else. It covers album, video and per-song art and the artist
		 * portraits, since an artist's cover art id is its folder id.
		 */
		const val ART_ENDPOINT = "getCoverArt"
	}
}
