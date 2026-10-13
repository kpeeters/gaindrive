package org.gaindrive.android

import android.app.Application
import android.util.Log
import coil3.ImageLoader
import coil3.PlatformContext
import coil3.SingletonImageLoader
import coil3.disk.DiskCache
import coil3.network.okhttp.OkHttpNetworkFetcherFactory
import coil3.request.crossfade
import dagger.hilt.android.HiltAndroidApp
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.launch
import okhttp3.OkHttpClient
import okio.Path.Companion.toOkioPath
import org.gaindrive.android.data.cache.AudioCache
import org.gaindrive.android.data.cache.PinRepository
import org.gaindrive.android.data.cache.PinnedArt
import org.gaindrive.android.data.cache.PinnedArtMapper
import javax.inject.Inject

@HiltAndroidApp
class GainDriveApplication : Application(), SingletonImageLoader.Factory {

	/**
	 * The same client every Retrofit instance is built from, so cover art
	 * shares its connection pool rather than opening a second one per server.
	 */
	@Inject
	lateinit var httpClient: OkHttpClient

	/**
	 * Injected the same way and safe for the same reason: [newImageLoader] is
	 * called lazily, on the first image request, long after `onCreate`.
	 */
	@Inject
	lateinit var pinnedArt: PinnedArt

	// Lazy, so that the start of every other epoch does not build them early.
	@Inject
	lateinit var audioCache: dagger.Lazy<AudioCache>

	@Inject
	lateinit var pins: dagger.Lazy<PinRepository>

	@Inject
	lateinit var appScope: CoroutineScope

	override fun onCreate() {
		// Before super, which is where Hilt injects the client whose HTTP cache
		// lives in cacheDir; the image loader is built later still.
		val newEpoch = clearStaleCaches()
		super.onCreate()
		if (newEpoch) flushStaleAudio()
	}

	/**
	 * Empties `cacheDir` once per [CACHE_EPOCH], as the system's "Clear cache"
	 * button would: covers and HTTP responses, nothing the user asked to keep.
	 * The library mirror is left alone because it shares its database with
	 * the pins, and browsing reads the network before it anyway.
	 *
	 * True when this start is the first of a new epoch.
	 */
	private fun clearStaleCaches(): Boolean {
		val marker = cacheDir.resolve("cache-epoch")
		val stored = runCatching { marker.readText().trim().toInt() }.getOrNull()
		if (stored == CACHE_EPOCH) return false
		cacheDir.listFiles()?.forEach { it.deleteRecursively() }
		runCatching { marker.writeText(CACHE_EPOCH.toString()) }
			.onFailure { Log.w(TAG, "Could not write the cache epoch marker", it) }
		Log.i(TAG, "Cleared cacheDir for cache epoch $CACHE_EPOCH (was $stored)")
		return true
	}

	/**
	 * The unpinned half of the music cache, which lives in `filesDir` beside
	 * the downloads. A partly cached track would otherwise be finished from a
	 * request the server now answers differently, splicing two encodings under
	 * one key. Downloads are complete files and are kept.
	 */
	private fun flushStaleAudio() {
		appScope.launch {
			pins.get().awaitProtection()
			audioCache.get().flush()
			Log.i(TAG, "Flushed unpinned audio for cache epoch $CACHE_EPOCH")
		}
	}

	override fun newImageLoader(context: PlatformContext): ImageLoader =
		ImageLoader.Builder(context)
			.components {
				// Before the fetcher, so a pinned file is served without a
				// request ever being built for it.
				add(PinnedArtMapper(pinnedArt))
				add(OkHttpNetworkFetcherFactory(callFactory = { httpClient }))
			}
			// Keyed by `ArtKeys` rather than by the URL, which carries a salt
			// that changes on every app start and so made every entry written
			// by the previous session unreachable. That is what lets this
			// survive a restart, which is the whole point of a disk cache.
			.diskCache {
				DiskCache.Builder()
					// Coil 3 speaks okio paths, not java.io.File.
					.directory(cacheDir.resolve("coil").toOkioPath())
					.maxSizeBytes(COVER_CACHE_BYTES)
					.build()
			}
			.crossfade(true)
			.build()

	private companion object {
		const val COVER_CACHE_BYTES = 128L * 1024 * 1024
		const val TAG = "GainDriveApplication"

		// Bump when a release must start with empty caches, e.g. because what
		// the server sends for the same request has changed. 1: the first
		// update after the per-player quality limit, which changed what the
		// app declares when it asks for audio.
		const val CACHE_EPOCH = 1
	}
}
