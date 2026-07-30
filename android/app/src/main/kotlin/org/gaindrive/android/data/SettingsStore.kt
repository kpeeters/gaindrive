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
	 * Off by default: the collapse hides one copy behind another on nothing
	 * more than an artist-and-title match, and a library that quietly omits
	 * something is worse than one that shows it twice. Users who have
	 * deliberately downloaded their streaming collection locally turn it on.
	 */
	val mergeDuplicateAlbums: Flow<Boolean> =
		dataStore.data.map { it[MERGE_ALBUMS] ?: false }

	suspend fun setMergeDuplicateAlbums(enabled: Boolean) {
		dataStore.edit { it[MERGE_ALBUMS] = enabled }
	}

	private companion object {
		val THEME = stringPreferencesKey("theme_mode")
		val SELECTED_SERVER = stringPreferencesKey("selected_server")
		val MERGE_ALBUMS = booleanPreferencesKey("merge_duplicate_albums")
	}
}
