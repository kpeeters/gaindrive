package org.gaindrive.android.data

import androidx.datastore.core.DataStore
import androidx.datastore.preferences.core.Preferences
import androidx.datastore.preferences.core.edit
import androidx.datastore.preferences.core.stringPreferencesKey
import kotlinx.coroutines.flow.Flow
import kotlinx.coroutines.flow.map
import kotlinx.serialization.Serializable
import kotlinx.serialization.decodeFromString
import kotlinx.serialization.encodeToString
import kotlinx.serialization.json.Json
import javax.inject.Inject
import javax.inject.Singleton

/**
 * One server as persisted. The password is already ciphertext by the time it
 * reaches here — see [org.gaindrive.android.data.crypto.CredentialCipher].
 *
 * Every field beyond the first four has a default so that adding fields later
 * (cast mode, LAN address) reads older documents without a migration. That is
 * not merely convenient: a decode failure falls back to an empty list below, so
 * a field added *without* a default would not fail loudly — it would silently
 * discard every configured server.
 */
@Serializable
data class StoredServer(
	val id: String,
	val name: String,
	val url: String,
	val username: String,
	val password: String = "",
	val enabled: Boolean = true,
	/** See [org.gaindrive.android.data.model.ServerConfig.browseByFolder]. */
	val browseByFolder: Boolean = false,
)

/**
 * Persists the server list as a JSON document in a Preferences DataStore.
 *
 * A handful of records does not justify Room. Phase 6's Room database is for
 * cached library metadata, which is a different concern with a different
 * lifetime — clearing that cache must not log anyone out.
 */
@Singleton
class ServerStore @Inject constructor(
	private val dataStore: DataStore<Preferences>,
	private val json: Json,
) {

	val servers: Flow<List<StoredServer>> = dataStore.data.map { prefs ->
		val raw = prefs[KEY] ?: return@map emptyList()
		// A corrupt document must not brick the app on every launch; an empty
		// list at least lets the user add a server again.
		runCatching { json.decodeFromString<List<StoredServer>>(raw) }.getOrDefault(emptyList())
	}

	suspend fun save(servers: List<StoredServer>) {
		val encoded = json.encodeToString(servers)
		dataStore.edit { it[KEY] = encoded }
	}

	private companion object {
		val KEY = stringPreferencesKey("servers")
	}
}
