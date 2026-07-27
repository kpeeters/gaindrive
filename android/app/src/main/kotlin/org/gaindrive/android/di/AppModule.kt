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
import kotlinx.serialization.json.Json
import okhttp3.Cache
import okhttp3.OkHttpClient
import java.util.concurrent.TimeUnit
import javax.inject.Singleton

@Module
@InstallIn(SingletonComponent::class)
object AppModule {

	@Provides
	@Singleton
	fun json(): Json = Json {
		// Servers add fields over time and OpenSubsonic extensions add more;
		// an unknown key must never fail a response.
		ignoreUnknownKeys = true
		// Tolerates nulls where the DTO declares a non-null default, which
		// some Subsonic implementations emit for absent values.
		coerceInputValues = true
	}

	@Provides
	@Singleton
	fun okHttp(@ApplicationContext context: Context): OkHttpClient = OkHttpClient.Builder()
		// Shared by every server's Retrofit and by Coil, so cover art benefits
		// from this long before the Phase 6 cache exists.
		.cache(Cache(context.cacheDir.resolve("http"), HTTP_CACHE_BYTES))
		.connectTimeout(15, TimeUnit.SECONDS)
		.readTimeout(30, TimeUnit.SECONDS)
		.build()

	@Provides
	@Singleton
	fun dataStore(@ApplicationContext context: Context): DataStore<Preferences> =
		PreferenceDataStoreFactory.create {
			context.preferencesDataStoreFile("gaindrive")
		}

	private const val HTTP_CACHE_BYTES = 32L * 1024 * 1024
}
