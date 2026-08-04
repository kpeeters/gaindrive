package org.gaindrive.android.di

import android.content.Context
import androidx.datastore.core.DataStore
import androidx.datastore.preferences.core.PreferenceDataStoreFactory
import androidx.datastore.preferences.core.Preferences
import androidx.datastore.preferences.preferencesDataStoreFile
import dagger.Module
import dagger.Provides
import dagger.hilt.InstallIn
import dagger.hilt.android.qualifiers.ApplicationContext
import dagger.hilt.components.SingletonComponent
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.SupervisorJob
import kotlinx.serialization.json.Json
import okhttp3.Cache
import okhttp3.OkHttpClient
import okhttp3.logging.HttpLoggingInterceptor
import org.gaindrive.android.BuildConfig
import org.gaindrive.android.net.AuthInterceptor
import org.gaindrive.android.net.SubsonicJson
import java.util.concurrent.TimeUnit
import javax.inject.Singleton

@Module
@InstallIn(SingletonComponent::class)
object AppModule {

	/**
	 * One [Json] for the whole app, configured where the tests can reach it.
	 *
	 * Its settings are argued in `SubsonicJson`, because reading other people's
	 * servers is the strictest constraint on it; the cast channel and the stored
	 * server list also use it, and neither cares — leniency is a decoding
	 * concession and what this app writes it also wrote.
	 */
	@Provides
	@Singleton
	fun json(): Json = SubsonicJson

	@Provides
	@Singleton
	fun okHttp(@ApplicationContext context: Context): OkHttpClient = OkHttpClient.Builder()
		// Shared by every server's Retrofit and by Coil, so cover art benefits
		// from this long before the Phase 6 cache exists.
		.cache(Cache(context.cacheDir.resolve("http"), HTTP_CACHE_BYTES))
		.connectTimeout(15, TimeUnit.SECONDS)
		.readTimeout(30, TimeUnit.SECONDS)
		// Says who is calling, in the one place that covers every caller: each
		// server's Retrofit, Coil's cover art, ExoPlayer's stream reads, the
		// download manager and the transcode pre-warm. OkHttp's default names
		// only the HTTP stack, which makes a server log unreadable the moment
		// more than one client is involved.
		//
		// `header`, not `addHeader`: this replaces rather than appends, and an
		// application interceptor runs before OkHttp's BridgeInterceptor, which
		// only supplies its own default when the header is absent.
		.addInterceptor { chain ->
			chain.proceed(
				chain.request().newBuilder()
					.header("User-Agent", USER_AGENT)
					.build()
			)
		}
		.apply {
			// Debug builds only: these URLs carry the auth token, so this must
			// never be enabled in a release build.
			if (BuildConfig.DEBUG) {
				addInterceptor(
					HttpLoggingInterceptor().setLevel(HttpLoggingInterceptor.Level.BODY)
				)
			}
		}
		.build()

	@Provides
	@Singleton
	fun dataStore(@ApplicationContext context: Context): DataStore<Preferences> =
		PreferenceDataStoreFactory.create {
			context.preferencesDataStoreFile("gaindrive")
		}

	/**
	 * Application-lifetime scope for work that outlives any screen — the player
	 * connection in particular, which must survive navigation.
	 *
	 * `Main.immediate` because most of it drives a [androidx.media3.session.MediaController],
	 * whose methods must be called on the main thread.
	 */
	@Provides
	@Singleton
	fun applicationScope(): CoroutineScope =
		CoroutineScope(SupervisorJob() + Dispatchers.Main.immediate)

	private const val HTTP_CACHE_BYTES = 32L * 1024 * 1024

	/**
	 * Built from [AuthInterceptor.CLIENT_NAME] rather than repeating the name,
	 * so the `c=` query parameter and the User-Agent cannot drift apart — a
	 * server reading one or the other should see the same client.
	 *
	 * Must not contain "Mozilla/": gaindrive tests the User-Agent for that
	 * substring to decide whether to pace a stream at playback rate, which is
	 * right for a browser's audio element and wrong for this app.
	 */
	private val USER_AGENT =
		"${AuthInterceptor.CLIENT_NAME}/${BuildConfig.VERSION_NAME} (okhttp)"
}
