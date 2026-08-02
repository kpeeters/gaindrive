package org.gaindrive.android.data

import androidx.datastore.core.DataStore
import androidx.datastore.preferences.core.Preferences
import androidx.datastore.preferences.core.booleanPreferencesKey
import androidx.datastore.preferences.core.edit
import androidx.datastore.preferences.core.longPreferencesKey
import androidx.datastore.preferences.core.stringPreferencesKey
import kotlinx.coroutines.flow.Flow
import kotlinx.coroutines.flow.map
import org.gaindrive.android.data.model.AudioQuality
import org.gaindrive.android.data.model.ThemeMode
import javax.inject.Inject
import javax.inject.Singleton

/**
 * App-wide preferences that are not tied to any one server.
 */
@Singleton
class SettingsStore @Inject constructor(
	private val dataStore: DataStore<Preferences>,
) {

	val themeMode: Flow<ThemeMode> = dataStore.data.map { prefs ->
		prefs[THEME]?.let { name -> runCatching { ThemeMode.valueOf(name) }.getOrNull() }
			?: ThemeMode.AUTO
	}

	suspend fun setThemeMode(mode: ThemeMode) {
		dataStore.edit { it[THEME] = mode.name }
	}

	/**
	 * What the library screens are showing: a server id, or
	 * [org.gaindrive.android.data.model.BrowseScope.ALL_STORED]. Null until the
	 * user has chosen; resolving it is [ServerSelection]'s job, since the stored
	 * choice may since have been disabled or removed.
	 */
	val browseScope: Flow<String?> = dataStore.data.map { it[SELECTED_SERVER] }

	/**
	 * Which kind of top-level entry the library view is showing.
	 *
	 * Stored as the raw content-type id rather than an enum ordinal: the modes
	 * come from the server, so a value written today may name a kind this
	 * build has never heard of, and it must survive a round trip regardless.
	 */
	val libraryMode: Flow<String?> = dataStore.data.map { it[LIBRARY_MODE] }

	suspend fun setLibraryMode(value: String) {
		dataStore.edit { it[LIBRARY_MODE] = value }
	}

	suspend fun setBrowseScope(value: String) {
		dataStore.edit { it[SELECTED_SERVER] = value }
	}

	/**
	 * Whether an album held on several servers collapses to one row.
	 *
	 * On by default. Anyone running two servers at once is likely to have the
	 * same album on both — a streaming collection beside locally downloaded
	 * copies of it — and seeing every one of them twice is the worse default.
	 * The badges on a collapsed row keep it honest about what was folded away.
	 */
	val mergeDuplicateAlbums: Flow<Boolean> =
		dataStore.data.map { it[MERGE_ALBUMS] ?: true }

	suspend fun setMergeDuplicateAlbums(enabled: Boolean) {
		dataStore.edit { it[MERGE_ALBUMS] = enabled }
	}

	/**
	 * How much audio the cache may hold. Pinned downloads can push it past this
	 * — see `PinAwareEvictor` — but nothing else may.
	 */
	val cacheMaxBytes: Flow<Long> =
		dataStore.data.map { it[CACHE_MAX_BYTES] ?: DEFAULT_CACHE_BYTES }

	suspend fun setCacheMaxBytes(bytes: Long) {
		dataStore.edit { it[CACHE_MAX_BYTES] = bytes }
	}

	/**
	 * Whether playing a track also stores it.
	 *
	 * On by default: replaying an album is the common case, and the cap plus
	 * automatic eviction means it cannot run away. Turning it off still leaves
	 * the cache readable, so pinned downloads keep working.
	 */
	val cacheOnPlay: Flow<Boolean> = dataStore.data.map { it[CACHE_ON_PLAY] ?: true }

	suspend fun setCacheOnPlay(enabled: Boolean) {
		dataStore.edit { it[CACHE_ON_PLAY] = enabled }
	}

	/**
	 * Behave as though there were no network, whatever the network says.
	 *
	 * A mode, not a display preference: it stops requests being made at all, so
	 * a slow or expensive connection costs nothing and no screen waits out a
	 * timeout before showing what is stored.
	 */
	val offlineMode: Flow<Boolean> = dataStore.data.map { it[OFFLINE_MODE] ?: false }

	suspend fun setOfflineMode(enabled: Boolean) {
		dataStore.edit { it[OFFLINE_MODE] = enabled }
	}

	/**
	 * Whether pinned downloads wait for an unmetered connection.
	 *
	 * On by default, and only applies to downloads: caching what you are
	 * already streaming costs no extra data, so gating that on Wi-Fi would
	 * penalise the mobile listener for nothing.
	 */
	val downloadUnmeteredOnly: Flow<Boolean> =
		dataStore.data.map { it[DOWNLOAD_UNMETERED_ONLY] ?: true }

	suspend fun setDownloadUnmeteredOnly(enabled: Boolean) {
		dataStore.edit { it[DOWNLOAD_UNMETERED_ONLY] = enabled }
	}

	/**
	 * What quality to fetch audio at — downloads, cache-on-play and plain
	 * streaming alike.
	 *
	 * One setting rather than one per connection type: everything played is
	 * cached, so a separate streaming quality would only mean storing something
	 * different from what a download of the same track produces, and then
	 * holding both.
	 *
	 * Stored as [AudioQuality.tag] so it is a single value — a format and a
	 * bitrate written separately could be observed half-applied.
	 */
	val audioQuality: Flow<AudioQuality> = dataStore.data.map { prefs ->
		prefs[AUDIO_QUALITY]?.let(AudioQuality::parse) ?: AudioQuality.DEFAULT
	}

	suspend fun setAudioQuality(quality: AudioQuality) {
		dataStore.edit { it[AUDIO_QUALITY] = quality.tag }
	}

	/**
	 * Whether the user has ever chosen a quality.
	 *
	 * Used once, at startup: someone upgrading with pins already downloaded
	 * should not silently have all of them re-fetched because the new default
	 * differs from what those bytes are. See `QualityMigration`.
	 */
	val audioQualityChosen: Flow<Boolean> =
		dataStore.data.map { it[AUDIO_QUALITY] != null }

	companion object {
		/** Big enough to be useful, small enough not to surprise anyone. */
		const val DEFAULT_CACHE_BYTES = 4L * 1024 * 1024 * 1024

		private val THEME = stringPreferencesKey("theme_mode")
		private val SELECTED_SERVER = stringPreferencesKey("selected_server")
		private val LIBRARY_MODE = stringPreferencesKey("library_mode")
		private val MERGE_ALBUMS = booleanPreferencesKey("merge_duplicate_albums")
		private val CACHE_MAX_BYTES = longPreferencesKey("cache_max_bytes")
		private val CACHE_ON_PLAY = booleanPreferencesKey("cache_on_play")
		private val DOWNLOAD_UNMETERED_ONLY = booleanPreferencesKey("download_unmetered_only")
		private val OFFLINE_MODE = booleanPreferencesKey("offline_mode")
		private val AUDIO_QUALITY = stringPreferencesKey("audio_quality")
	}
}
