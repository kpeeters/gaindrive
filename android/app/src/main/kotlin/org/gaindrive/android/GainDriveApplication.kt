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
import javax.inject.Inject

@HiltAndroidApp
class GainDriveApplication : Application(), SingletonImageLoader.Factory {

	/**
	 * The same client every Retrofit instance is built from, so cover art
	 * shares its connection pool rather than opening a second one per server.
	 */
	@Inject
	lateinit var httpClient: OkHttpClient

	override fun newImageLoader(context: PlatformContext): ImageLoader =
		ImageLoader.Builder(context)
			.components {
				add(OkHttpNetworkFetcherFactory(callFactory = { httpClient }))
			}
			// Cover art URLs are stable — the auth salt is per session, not per
			// request — so a disk cache actually earns its keep here.
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
