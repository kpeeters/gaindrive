package org.gaindrive.android.ui.player

import androidx.lifecycle.ViewModel
import androidx.lifecycle.viewModelScope
import dagger.hilt.android.lifecycle.HiltViewModel
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.asStateFlow
import kotlinx.coroutines.flow.first
import kotlinx.coroutines.launch
import org.gaindrive.android.data.EqStore
import org.gaindrive.android.net.runCatchingCancellable
import org.gaindrive.android.net.userMessage
import org.gaindrive.android.playback.cast.CastDevice
import org.gaindrive.android.playback.cast.CastSession
import org.gaindrive.android.playback.wiim.WIIM_BANDS
import org.gaindrive.android.playback.wiim.WIIM_LEVEL_MAX
import org.gaindrive.android.playback.wiim.WIIM_LEVEL_MIN
import org.gaindrive.android.playback.wiim.WiiMClient
import org.gaindrive.android.ui.Load
import java.io.IOException
import javax.inject.Inject

/** What the equalizer section draws: the device's state and what it offers. */
data class WiiMEqUi(
	val enabled: Boolean,
	/** Null for a hand-shaped curve, or when the device would not say. */
	val preset: String?,
	val presets: List<String>,
	/**
	 * Fader positions in `WIIM_BANDS` order, or null when the device reported
	 * no usable set; the sheet then offers the switch and the preset list, the
	 * feature set every firmware has.
	 */
	val bands: List<Int>?,
	/** The presets saved on this phone for this device, by name. */
	val saved: List<String>,
)

/**
 * Backs [WiiMControlsSheet].
 *
 * [CastSession] is injected directly and its device re-exported, the way
 * [TrackInfoViewModel] does it: a dialog's view model needs the connected
 * device, and reaching for [CastViewModel] to get it would tie this surface to
 * the picker's.
 *
 * Saved presets live in [EqStore] under [CastDevice.id], not on the device:
 * the WiiM's own preset store has no documented write, and the id survives a
 * rename and a new DHCP lease, which the address does not. The consequence is
 * that the device cannot know a saved preset's name, so the name shown is
 * this class's bookkeeping, not the device's `Name` field, which anyway keeps
 * naming the last `EQLoad` after the band values have moved on.
 */
@HiltViewModel
class WiiMControlsViewModel @Inject constructor(
	private val client: WiiMClient,
	private val store: EqStore,
	castSession: CastSession,
) : ViewModel() {

	val device: StateFlow<CastDevice?> = castSession.device

	private val _state = MutableStateFlow<Load<WiiMEqUi>>(Load.Loading)
	val state: StateFlow<Load<WiiMEqUi>> = _state.asStateFlow()

	/** A command in flight; the section disables itself meanwhile. */
	private val _busy = MutableStateFlow(false)
	val busy: StateFlow<Boolean> = _busy.asStateFlow()

	private val _error = MutableStateFlow<String?>(null)
	val error: StateFlow<String?> = _error.asStateFlow()

	/**
	 * The saved curves, mirrored from the store the way `EqualizerController`
	 * mirrors its slots: applying one must not wait on a flow, and nothing
	 * else writes this device's slots while the sheet is open.
	 */
	private var slots: Map<String, List<Int>> = emptyMap()

	/**
	 * Reads the device's state and the presets it offers.
	 *
	 * Not idempotent, unlike [AddToPlaylistViewModel.load]: the EQ can be changed
	 * from the WiiM app or the device itself between openings, and this sheet is
	 * cheap to fill. Nothing polls - a LAN round trip on a timer is not worth
	 * what it would buy.
	 */
	fun refresh() {
		val target = device.value ?: run {
			_state.value = Load.Failed("Not connected to a WiiM.")
			return
		}
		_state.value = Load.Loading
		viewModelScope.launch {
			_state.value = runCatchingCancellable {
				// State first: it is the reachability test, which is what lets
				// `presets` fall back to the documented list without hiding a
				// device that is simply not there.
				val eq = client.eqState(target.address)
				val presets = client.presets(target.address)
				// A wrong-sized curve could only come from a corrupted store,
				// and could not be written to the device anyway.
				slots = store.slots(target.id).first()
					.filterValues { it.size == WIIM_BANDS.size }
				val storedLevels = store.levelsMb(target.id).first()
				val storedName = store.preset(target.id).first()
				// The device cannot know a phone-side preset's name, and its
				// `Name` field keeps naming the last `EQLoad` after the bands
				// have moved. So when the device's curve is exactly the one
				// this phone last wrote, the phone's record of where it came
				// from wins (a saved slot, or null for hand-shaped); any other
				// curve means the EQ was changed elsewhere, and the device's
				// own name is the best answer available.
				val preset = if (eq.bands != null && eq.bands == storedLevels) {
					storedName?.takeIf { it in presets || it in slots }
				} else {
					eq.preset
				}
				WiiMEqUi(
					enabled = eq.enabled,
					preset = preset,
					presets = presets,
					bands = eq.bands,
					saved = slots.keys.sorted(),
				)
			}.fold(
				onSuccess = { Load.Ready(it) },
				onFailure = { Load.Failed(it.userMessage()) },
			)
		}
	}

	/**
	 * Loads a preset, switching the equalizer on with it.
	 *
	 * The `EQOn` is deliberate and not redundant: the WiiM documentation does not
	 * say whether `EQLoad` enables the equalizer, and tapping "Rock" while it is
	 * off must not be a silent no-op. Sending both makes the outcome the same
	 * either way.
	 */
	fun selectPreset(preset: String) = mutate(
		failure = "Could not load that preset",
		optimistic = { it.copy(enabled = true, preset = preset) },
	) { address ->
		if (!client.loadPreset(address, preset)) {
			throw IOException("The device would not load that preset.")
		}
		client.setEqEnabled(address, true)
	}

	/** Applies a curve saved on this phone; the `EQOn` for [selectPreset]'s reason. */
	fun applySaved(name: String) {
		val levels = slots[name] ?: return
		mutate(
			failure = "Could not apply that preset",
			optimistic = { it.copy(enabled = true, preset = name, bands = levels) },
		) { address ->
			if (!client.setBands(address, levels)) {
				throw IOException("The device would not change the equalizer.")
			}
			client.setEqEnabled(address, true)
		}
	}

	fun setEnabled(enabled: Boolean) = mutate(
		failure = if (enabled) "Could not switch the equalizer on" else "Could not switch it off",
		optimistic = { it.copy(enabled = enabled) },
	) { address ->
		if (!client.setEqEnabled(address, enabled)) {
			throw IOException("The device would not change the equalizer.")
		}
	}

	/**
	 * One fader moving under the finger. Local only, like
	 * `EqualizerController.setBandLevel`: a LAN round trip per drag tick would
	 * saturate the device, and [commitBands] runs when the finger lifts.
	 */
	fun setBand(index: Int, value: Int) {
		val ui = ready() ?: return
		val bands = ui.bands ?: return
		if (index !in bands.indices) return
		val next = bands.toMutableList()
			.also { it[index] = value.coerceIn(WIIM_LEVEL_MIN, WIIM_LEVEL_MAX) }
		// A moved fader is no longer any named preset's curve.
		_state.value = Load.Ready(ui.copy(bands = next, preset = null))
	}

	/** Sends the dragged curve to the device; the slider's finger-up callback. */
	fun commitBands() {
		val bands = ready()?.bands ?: return
		mutate(
			failure = "Could not change the equalizer",
			optimistic = { it.copy(preset = null) },
		) { address ->
			if (!client.setBands(address, bands)) {
				throw IOException("The device would not change the equalizer.")
			}
		}
	}

	/**
	 * Saves the current curve under [name], on this phone. Returns whether it
	 * was accepted; a refusal lands in [error]. The rules are
	 * `EqualizerController.saveSlot`'s: a saved preset may be overwritten
	 * silently, a device preset's name may not be taken.
	 */
	fun save(name: String): Boolean {
		val ui = ready() ?: return false
		val bands = ui.bands ?: return false
		val key = device.value?.id ?: return false
		val trimmed = name.trim()
		if (trimmed.isEmpty()) {
			_error.value = "A preset needs a name."
			return false
		}
		if (trimmed in ui.presets) {
			_error.value = "“$trimmed” is already one of the device's presets."
			return false
		}
		// Optimistic, so the row is not a step behind the store.
		slots = slots + (trimmed to bands)
		_state.value = Load.Ready(ui.copy(preset = trimmed, saved = slots.keys.sorted()))
		viewModelScope.launch {
			store.saveSlot(trimmed, bands, key)
			store.setCurve(bands, trimmed, key)
		}
		return true
	}

	/** Deletes the selected saved preset; the curve stays, the name drops. */
	fun deleteSlot() {
		val ui = ready() ?: return
		val name = ui.preset?.takeIf { it in slots } ?: return
		val key = device.value?.id ?: return
		slots = slots - name
		_state.value = Load.Ready(ui.copy(preset = null, saved = slots.keys.sorted()))
		viewModelScope.launch {
			store.deleteSlot(name, key)
			ui.bands?.let { store.setCurve(it, null, key) }
		}
	}

	fun clearError() {
		_error.value = null
	}

	private fun ready(): WiiMEqUi? = (_state.value as? Load.Ready)?.value

	/**
	 * Applies a change locally, sends it, then publishes what the device says it
	 * actually did.
	 *
	 * The optimistic step is what makes a radio button move under the finger
	 * rather than after a round trip. It is put back on failure, because a
	 * control that shows a state the device is not in is worse than a slow one.
	 *
	 * The re-read contributes the switch and the band values but never the
	 * preset name: the device keeps naming the last `EQLoad` after the bands
	 * have moved (measured), and it cannot know a phone-side preset's name at
	 * all, so after a mutation the optimistic name is the truth. The curve is
	 * then recorded in the store, which is what lets [refresh] hand the name
	 * back on the next opening.
	 */
	private fun mutate(
		failure: String,
		optimistic: (WiiMEqUi) -> WiiMEqUi,
		block: suspend (String) -> Unit,
	) {
		val target = device.value ?: return
		if (_busy.value) return
		val before = _state.value
		val guessed = (before as? Load.Ready)?.value?.let(optimistic) ?: return
		_state.value = Load.Ready(guessed)
		_busy.value = true
		viewModelScope.launch {
			runCatchingCancellable {
				block(target.address)
				client.eqState(target.address)
			}.fold(
				onSuccess = { eq ->
					val bands = eq.bands ?: guessed.bands
					_state.value = Load.Ready(guessed.copy(enabled = eq.enabled, bands = bands))
					bands?.let { store.setCurve(it, guessed.preset, target.id) }
				},
				onFailure = {
					_state.value = before
					_error.value = "$failure: ${it.userMessage()}"
				},
			)
			_busy.value = false
		}
	}
}
