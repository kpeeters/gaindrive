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
import org.gaindrive.android.data.cache.PinAwareEvictor
import org.gaindrive.android.data.cache.PinnedKeys
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
	 * [MediaHttp] rather than the shared client: a pinned film's soundtrack is
	 * built by a blocking transcode on the server, and 30 s is not enough to
	 * wait for one.
	 */
	@Provides
	@Singleton
	fun downloadManager(
		@ApplicationContext context: Context,
		databaseProvider: DatabaseProvider,
		cache: SimpleCache,
		@MediaHttp httpClient: OkHttpClient,
	): DownloadManager = DownloadManager(
		context,
		DefaultDownloadIndex(databaseProvider),
		DefaultDownloaderFactory(
			CacheDataSource.Factory()
				.setCache(cache)
				.setUpstreamDataSourceFactory(OkHttpDataSource.Factory(httpClient)),
			Executors.newFixedThreadPool(DOWNLOAD_THREADS),
		),
	).apply { maxParallelDownloads = DOWNLOAD_THREADS }

	private const val DOWNLOAD_THREADS = 2
}
