package org.gaindrive.android.data

import androidx.datastore.core.DataStore
import androidx.datastore.preferences.core.Preferences
import androidx.datastore.preferences.core.edit
import androidx.datastore.preferences.core.stringPreferencesKey
import kotlinx.coroutines.flow.Flow
import kotlinx.coroutines.flow.first
import kotlinx.coroutines.flow.map
import kotlinx.serialization.Serializable
import kotlinx.serialization.decodeFromString
import kotlinx.serialization.encodeToString
import kotlinx.serialization.json.Json
import org.gaindrive.android.playback.cast.CastDevice
import java.util.UUID
import javax.inject.Inject
import javax.inject.Singleton

/** The Cast v2 control port. Nobody should ever need to change it. */
const val DEFAULT_CAST_PORT = 8009

/**
 * A Chromecast the user named by address rather than one discovery found.
 *
 * [id] is a generated UUID fixed when the device is added, not derived from the
 * address, so renaming or re-addressing an entry stays an edit of the same
 * device. Note that [CastDevice] is a plain data class and
 * [org.gaindrive.android.playback.cast.CastSession.connect] compares targets
 * structurally, so editing a device while it is connected reads as a different
 * device and reconnects. That is the honest behaviour for a changed address and
 * merely a reconnect for a changed name.
 *
 * Every field after [address] has a default, for the reason spelled out on
 * [StoredServer]: the decode below falls back to an empty list, so a field added
 * without one would silently discard the whole list instead of failing loudly.
 */
@Serializable
data class ManualCastDevice(
	val id: String,
	val address: String,
	/** Blank until the user types one, or a probe learns it; falls back to [address]. */
	val name: String = "",
	val port: Int = DEFAULT_CAST_PORT,
) {
	companion object {
		fun create(address: String, name: String, port: Int) = ManualCastDevice(
			id = UUID.randomUUID().toString(),
			address = address,
			name = name,
			port = port,
		)
	}
}

/** What the rest of casting speaks. A blank name would render as an empty row. */
fun ManualCastDevice.toCastDevice(): CastDevice = CastDevice(
	id = id,
	name = name.ifBlank { address },
	address = address,
	port = port,
)

/**
 * Persists manually added Cast devices, as [ServerStore] persists servers: one
 * JSON document in a Preferences DataStore.
 *
 * This is the escape hatch for a device discovery cannot see.
 * [org.gaindrive.android.playback.cast.CastDiscovery] already records that
 * `NsdManager` is flaky across OEM builds; the case that prompted this was
 * worse than flaky, a device answering ping and a TLS connection on 8009 while
 * its mDNS responder had stopped answering even a direct unicast query. No
 * client can discover that, and no second discovery stack would have helped.
 *
 * Unlike servers there is no registry class over the top. That split exists so
 * [ServerRegistry] can bump a revision and invalidate library caches; nothing
 * caches against a Cast device, so the mutators live here.
 */
@Singleton
class CastDeviceStore @Inject constructor(
	private val dataStore: DataStore<Preferences>,
	private val json: Json,
) {

	val devices: Flow<List<ManualCastDevice>> = dataStore.data.map { prefs ->
		val raw = prefs[KEY] ?: return@map emptyList()
		// A corrupt document must not brick the picker on every launch.
		runCatching {
			json.decodeFromString<List<ManualCastDevice>>(raw)
		}.getOrDefault(emptyList())
	}

	suspend fun save(devices: List<ManualCastDevice>) {
		val encoded = json.encodeToString(devices)
		dataStore.edit { it[KEY] = encoded }
	}

	suspend fun add(device: ManualCastDevice) = mutate { it + device }

	suspend fun update(device: ManualCastDevice) = mutate { current ->
		current.map { if (it.id == device.id) device else it }
	}

	suspend fun remove(id: String) = mutate { current -> current.filterNot { it.id == id } }

	/**
	 * Read, apply, write - the idiom [ServerRegistry] uses. Writing back an
	 * unchanged list would wake every collector for nothing.
	 */
	private suspend fun mutate(block: (List<ManualCastDevice>) -> List<ManualCastDevice>) {
		val current = devices.first()
		val next = block(current)
		if (next == current) return
		save(next)
	}

	private companion object {
		val KEY = stringPreferencesKey("manual_cast_devices")
	}
}
