package org.gaindrive.android

import android.app.Application
import coil3.ImageLoader
import coil3.PlatformContext
import coil3.SingletonImageLoader
import coil3.disk.DiskCache
import coil3.network.okhttp.OkHttpNetworkFetcherFactory
import coil3.request.crossfade
import dagger.hilt.android.HiltAndroidApp
import okhttp3.OkHttpClient
import okio.Path.Companion.toOkioPath
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
	}
}
