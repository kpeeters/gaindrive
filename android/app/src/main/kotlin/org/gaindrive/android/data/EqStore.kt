package org.gaindrive.android.data

import android.util.Log
import androidx.datastore.core.DataStore
import androidx.datastore.preferences.core.Preferences
import androidx.datastore.preferences.core.booleanPreferencesKey
import androidx.datastore.preferences.core.edit
import androidx.datastore.preferences.core.stringPreferencesKey
import kotlinx.coroutines.flow.Flow
import kotlinx.coroutines.flow.map
import kotlinx.serialization.SerializationException
import kotlinx.serialization.decodeFromString
import kotlinx.serialization.encodeToString
import kotlinx.serialization.json.Json
import javax.inject.Inject
import javax.inject.Singleton

/**
 * Equalizer settings, keyed per output device.
 *
 * Per device rather than app-wide, because the same curve makes little sense
 * on two different sinks: each device keeps its own settings in its own band
 * layout. Only [LOCAL] exists today; a WiiM's settings would live under its
 * device id, and the requested per-album refinement can suffix these keys
 * without disturbing them.
 */
@Singleton
class EqStore @Inject constructor(
	private val dataStore: DataStore<Preferences>,
	private val json: Json,
) {

	fun enabled(device: String = LOCAL): Flow<Boolean> =
		dataStore.data.map { it[enabledKey(device)] ?: false }

	suspend fun setEnabled(enabled: Boolean, device: String = LOCAL) {
		dataStore.edit { it[enabledKey(device)] = enabled }
	}

	/**
	 * Band levels in millibels, one per band in band order. Null when unset
	 * or unreadable; the caller then starts flat, which for an equalizer is
	 * the harmless reading of a broken value.
	 */
	fun levelsMb(device: String = LOCAL): Flow<List<Int>?> =
		dataStore.data.map { prefs ->
			prefs[levelsKey(device)]?.let { raw ->
				val levels = raw.split(',').map { it.toIntOrNull() }
				if (levels.any { it == null }) {
					Log.w(TAG, "stored levels for $device are unreadable: $raw")
					null
				} else {
					levels.filterNotNull()
				}
			}
		}

	/**
	 * The device preset the curve came from, null when hand-shaped. Stored by
	 * name rather than index, so a preset table reordered by an OS update
	 * degrades to "custom" instead of to the wrong preset.
	 */
	fun preset(device: String = LOCAL): Flow<String?> =
		dataStore.data.map { it[presetKey(device)] }

	/**
	 * Levels and preset together in one edit: a curve and the name it came
	 * from are one fact, and writing them separately would let a kill land
	 * between the two.
	 */
	suspend fun setCurve(levelsMb: List<Int>, preset: String?, device: String = LOCAL) {
		dataStore.edit { prefs ->
			prefs[levelsKey(device)] = levelsMb.joinToString(",")
			if (preset == null) prefs.remove(presetKey(device))
			else prefs[presetKey(device)] = preset
		}
	}

	/**
	 * The user's saved presets: name to millibel levels, in band order. Like
	 * everything else here they are per device, since the curves are shaped
	 * in that device's own band layout.
	 */
	fun slots(device: String = LOCAL): Flow<Map<String, List<Int>>> =
		dataStore.data.map { prefs ->
			prefs[slotsKey(device)]?.let { decodeSlots(it, device) } ?: emptyMap()
		}

	/** Saves under [name], overwriting a slot that already carries it. */
	suspend fun saveSlot(name: String, levelsMb: List<Int>, device: String = LOCAL) {
		dataStore.edit { prefs ->
			val slots = prefs[slotsKey(device)]?.let { decodeSlots(it, device) } ?: emptyMap()
			prefs[slotsKey(device)] = json.encodeToString(slots + (name to levelsMb))
		}
	}

	suspend fun deleteSlot(name: String, device: String = LOCAL) {
		dataStore.edit { prefs ->
			val slots = prefs[slotsKey(device)]?.let { decodeSlots(it, device) } ?: return@edit
			val next = slots - name
			if (next.isEmpty()) prefs.remove(slotsKey(device))
			else prefs[slotsKey(device)] = json.encodeToString(next)
		}
	}

	private fun decodeSlots(raw: String, device: String): Map<String, List<Int>> =
		try {
			json.decodeFromString<Map<String, List<Int>>>(raw)
		} catch (e: SerializationException) {
			// Starting over is the harmless reading here too: a preset can be
			// saved again, a crash loop cannot be listened around.
			Log.w(TAG, "saved presets for $device are unreadable", e)
			emptyMap()
		}

	companion object {
		/** The phone itself, this app's only local output device. */
		const val LOCAL = "local"

		private const val TAG = "GainDriveEq"

		private fun enabledKey(device: String) = booleanPreferencesKey("eq_enabled_$device")
		private fun levelsKey(device: String) = stringPreferencesKey("eq_levels_$device")
		private fun presetKey(device: String) = stringPreferencesKey("eq_preset_$device")
		private fun slotsKey(device: String) = stringPreferencesKey("eq_slots_$device")
	}
}
