package org.gaindrive.android.data

import androidx.datastore.core.DataStore
import androidx.datastore.preferences.core.Preferences
import androidx.datastore.preferences.core.booleanPreferencesKey
import androidx.datastore.preferences.core.edit
import androidx.datastore.preferences.core.stringPreferencesKey
import kotlinx.coroutines.flow.Flow
import kotlinx.coroutines.flow.map
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

	private companion object {
		val THEME = stringPreferencesKey("theme_mode")
		val SELECTED_SERVER = stringPreferencesKey("selected_server")
		val MERGE_ALBUMS = booleanPreferencesKey("merge_duplicate_albums")
	}
}
