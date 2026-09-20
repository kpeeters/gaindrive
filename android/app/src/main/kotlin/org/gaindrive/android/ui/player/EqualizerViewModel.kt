package org.gaindrive.android.ui.player

import androidx.lifecycle.ViewModel
import androidx.lifecycle.viewModelScope
import dagger.hilt.android.lifecycle.HiltViewModel
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.SharingStarted
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.asStateFlow
import kotlinx.coroutines.flow.map
import kotlinx.coroutines.flow.stateIn
import org.gaindrive.android.playback.EqState
import org.gaindrive.android.playback.EqualizerController
import org.gaindrive.android.ui.Load
import javax.inject.Inject

/**
 * Backs [EqualizerSheet]: a thin adapter over [EqualizerController], which
 * owns the effect and outlives this sheet; the settings keep shaping sound
 * with the sheet closed. No busy flag, unlike [WiiMControlsViewModel]: the
 * effect answers synchronously, there is no round trip to wait out.
 */
@HiltViewModel
class EqualizerViewModel @Inject constructor(
	private val controller: EqualizerController,
) : ViewModel() {

	/**
	 * NoPlayer maps to Loading rather than to a failure: the sheet only opens
	 * from Now Playing, so a player exists and the effect is a beat away.
	 */
	val state: StateFlow<Load<EqState.Ready>> = controller.state
		.map { state ->
			when (state) {
				is EqState.NoPlayer -> Load.Loading
				is EqState.Unavailable -> Load.Failed(state.message)
				is EqState.Ready -> Load.Ready(state)
			}
		}
		.stateIn(viewModelScope, SharingStarted.Eagerly, Load.Loading)

	/** Why a save was refused, until the next keystroke clears it. */
	private val _error = MutableStateFlow<String?>(null)
	val error: StateFlow<String?> = _error.asStateFlow()

	init {
		controller.open()
	}

	fun setEnabled(enabled: Boolean) = controller.setEnabled(enabled)

	fun setBandLevel(band: Int, levelMb: Int) = controller.setBandLevel(band, levelMb)

	fun commitLevels() = controller.commitLevels()

	fun usePreset(index: Int) = controller.usePreset(index)

	fun useSaved(name: String) = controller.useSaved(name)

	/** True when the save was accepted, so the sheet can put the row away. */
	fun save(name: String): Boolean {
		val refusal = controller.saveSlot(name)
		_error.value = refusal
		return refusal == null
	}

	fun deleteSlot() = controller.deleteSlot()

	fun clearError() {
		_error.value = null
	}
}
