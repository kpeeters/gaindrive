package org.gaindrive.android.ui.player

import androidx.lifecycle.ViewModel
import androidx.lifecycle.viewModelScope
import dagger.hilt.android.lifecycle.HiltViewModel
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.asStateFlow
import kotlinx.coroutines.launch
import org.gaindrive.android.net.runCatchingCancellable
import org.gaindrive.android.net.userMessage
import org.gaindrive.android.playback.cast.CastDevice
import org.gaindrive.android.playback.cast.CastSession
import org.gaindrive.android.playback.wiim.WiiMClient
import org.gaindrive.android.ui.Load
import java.io.IOException
import javax.inject.Inject

/** What the equalizer section draws: the device's state and what it offers. */
data class WiiMEqUi(
	val enabled: Boolean,
	/** Null when the device would not say — see `WiiMEq.parseEqStat`. */
	val preset: String?,
	val presets: List<String>,
)

/**
 * Backs [WiiMControlsSheet].
 *
 * [CastSession] is injected directly and its device re-exported, the way
 * [TrackInfoViewModel] does it: a dialog's view model needs the connected
 * device, and reaching for [CastViewModel] to get it would tie this surface to
 * the picker's.
 */
@HiltViewModel
class WiiMControlsViewModel @Inject constructor(
	private val client: WiiMClient,
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
	 * Reads the device's state and the presets it offers.
	 *
	 * Not idempotent, unlike [AddToPlaylistViewModel.load]: the EQ can be changed
	 * from the WiiM app or the device itself between openings, and this sheet is
	 * cheap to fill. Nothing polls — a LAN round trip on a timer is not worth
	 * what it would buy.
	 */
	fun refresh() {
		val address = device.value?.address ?: run {
			_state.value = Load.Failed("Not connected to a WiiM.")
			return
		}
		_state.value = Load.Loading
		viewModelScope.launch {
			_state.value = runCatchingCancellable {
				// State first: it is the reachability test, which is what lets
				// `presets` fall back to the documented list without hiding a
				// device that is simply not there.
				val eq = client.eqState(address)
				WiiMEqUi(enabled = eq.enabled, preset = eq.preset, presets = client.presets(address))
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

	fun setEnabled(enabled: Boolean) = mutate(
		failure = if (enabled) "Could not switch the equalizer on" else "Could not switch it off",
		optimistic = { it.copy(enabled = enabled) },
	) { address ->
		if (!client.setEqEnabled(address, enabled)) {
			throw IOException("The device would not change the equalizer.")
		}
	}

	fun clearError() {
		_error.value = null
	}

	/**
	 * Applies a change locally, sends it, then publishes what the device says it
	 * actually did.
	 *
	 * The optimistic step is what makes a radio button move under the finger
	 * rather than after a round trip. It is put back on failure, because a
	 * control that shows a state the device is not in is worse than a slow one.
	 */
	private fun mutate(
		failure: String,
		optimistic: (WiiMEqUi) -> WiiMEqUi,
		block: suspend (String) -> Unit,
	) {
		val address = device.value?.address ?: return
		if (_busy.value) return
		val before = _state.value
		val guessed = (before as? Load.Ready)?.value?.let(optimistic) ?: return
		_state.value = Load.Ready(guessed)
		_busy.value = true
		viewModelScope.launch {
			runCatchingCancellable {
				block(address)
				client.eqState(address)
			}.fold(
				onSuccess = { eq ->
					_state.value = Load.Ready(
						guessed.copy(
							enabled = eq.enabled,
							// A device on the EQGetStat fallback reports no name,
							// and the guess is better than nothing there: we know
							// which preset was just sent.
							preset = eq.preset ?: guessed.preset,
						)
					)
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
