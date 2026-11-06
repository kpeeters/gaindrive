package org.gaindrive.android.di

import android.content.Context
import androidx.media3.database.DatabaseProvider
import androidx.media3.database.StandaloneDatabaseProvider
import androidx.media3.datasource.cache.CacheDataSource
import androidx.media3.datasource.cache.SimpleCache
import androidx.media3.datasource.okhttp.OkHttpDataSource
import androidx.media3.exoplayer.offline.DefaultDownloadIndex
import androidx.media3.exoplayer.offline.DefaultDownloaderFactory
import androidx.media3.exoplayer.offline.DownloadManager
import dagger.Module
import dagger.Provides
import dagger.hilt.InstallIn
import dagger.hilt.android.qualifiers.ApplicationContext
import dagger.hilt.components.SingletonComponent
import okhttp3.OkHttpClient
import org.gaindrive.android.data.SettingsStore
import org.gaindrive.android.data.cache.HlsKeys
import org.gaindrive.android.data.cache.PinAwareEvictor
import org.gaindrive.android.data.cache.PinnedKeys
import org.gaindrive.android.data.cache.RangeDownloaderFactory
import java.io.File
import java.util.concurrent.Executors
import javax.inject.Singleton

/**
 * The audio cache and the pieces it is built from.
 *
 * All singletons, and that is a hard requirement rather than a preference:
 * Media3 permits exactly one [SimpleCache] per directory per process, and a
 * second one would corrupt the first one's index.
 */
@Module
@InstallIn(SingletonComponent::class)
object CacheModule {

	@Provides
	@Singleton
	fun databaseProvider(@ApplicationContext context: Context): DatabaseProvider =
		StandaloneDatabaseProvider(context)

	@Provides
	@Singleton
	fun cacheEvictor(pinned: PinnedKeys): PinAwareEvictor =
		// The stored cap is applied by AudioCache as soon as DataStore answers;
		// this is only what holds until then.
		PinAwareEvictor(SettingsStore.DEFAULT_CACHE_BYTES, pinned)

	/**
	 * Lives in `filesDir`, not `cacheDir`.
	 *
	 * The system empties `cacheDir` under storage pressure, which would delete
	 * pinned downloads without warning - precisely when someone is offline and
	 * relying on them. The cost is that Android's "clear cache" no longer
	 * touches this, which is why Settings has a flush of its own.
	 */
	@Provides
	@Singleton
	fun mediaCache(
		@ApplicationContext context: Context,
		evictor: PinAwareEvictor,
		databaseProvider: DatabaseProvider,
	): SimpleCache = SimpleCache(File(context.filesDir, "media"), evictor, databaseProvider)

	/**
	 * Writes into the same cache playback reads from, through the same OkHttp.
	 *
	 * Two parallel downloads: enough that a slow track does not hold up an
	 * album, few enough that pinning something does not swamp the connection
	 * the user is streaming over at the same time.
	 *
	 * [MediaHttp] rather than the shared client: a transcode is built in full
	 * on the server before its first byte is sent, and 30 s is not enough to
	 * wait for a long one.
	 *
	 * A film's soundtrack is stored as HLS segments, a range at a time, by
	 * [RangeDownloaderFactory]; songs go to Media3's own factory as before.
	 * Both write through the same cache factory, so both file their bytes
	 * under the keys playback looks for ([HlsKeys]).
	 */
	@Provides
	@Singleton
	fun downloadManager(
		@ApplicationContext context: Context,
		databaseProvider: DatabaseProvider,
		cache: SimpleCache,
		@MediaHttp httpClient: OkHttpClient,
		pinned: PinnedKeys,
		hlsKeys: HlsKeys,
	): DownloadManager {
		val cacheFactory = CacheDataSource.Factory()
			.setCache(cache)
			.setCacheKeyFactory(hlsKeys.factory)
			.setUpstreamDataSourceFactory(OkHttpDataSource.Factory(httpClient))
		return DownloadManager(
			context,
			DefaultDownloadIndex(databaseProvider),
			RangeDownloaderFactory(
				fallback = DefaultDownloaderFactory(
					cacheFactory,
					Executors.newFixedThreadPool(DOWNLOAD_THREADS),
				),
				cacheFactory = cacheFactory,
				pinned = pinned,
				keys = hlsKeys,
			),
		).apply { maxParallelDownloads = DOWNLOAD_THREADS }
	}

	private const val DOWNLOAD_THREADS = 2
}
