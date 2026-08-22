package org.gaindrive.android.ui.settings

import androidx.lifecycle.ViewModel
import androidx.lifecycle.viewModelScope
import dagger.hilt.android.lifecycle.HiltViewModel
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.SharingStarted
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.asStateFlow
import kotlinx.coroutines.flow.stateIn
import kotlinx.coroutines.flow.update
import kotlinx.coroutines.launch
import org.gaindrive.android.data.CastDeviceStore
import org.gaindrive.android.data.DEFAULT_CAST_PORT
import org.gaindrive.android.data.ManualCastDevice
import org.gaindrive.android.data.toCastDevice
import org.gaindrive.android.playback.cast.CastProbe
import org.gaindrive.android.playback.cast.CastProbeResult
import javax.inject.Inject

/**
 * The add/edit dialog's contents. Null [editing] means the dialog is closed;
 * a non-null one holds the id being edited, or null inside [CastDeviceDraft]
 * for a device being added.
 */
data class CastDeviceDraft(
	/** Null when adding. The id is never edited, only carried. */
	val id: String? = null,
	val address: String = "",
	val name: String = "",
	val port: String = DEFAULT_CAST_PORT.toString(),
	val testing: Boolean = false,
	val testResult: CastProbeResult? = null,
) {
	val isNew: Boolean get() = id == null

	/**
	 * A port typed as text so the field can be empty mid-edit; anything that is
	 * not a plausible port blocks saving rather than being silently coerced,
	 * since a wrong port is indistinguishable from an unreachable device later.
	 */
	val portOrNull: Int? get() = port.toIntOrNull()?.takeIf { it in 1..65535 }

	val canSave: Boolean get() = address.isNotBlank() && portOrNull != null
}

/**
 * The manually added Cast devices, and the dialog that edits one.
 *
 * Its own view model rather than more state on [SettingsViewModel], which has
 * no room left: that class combines five flows at both of its levels and says
 * so in its own comments. There is precedent for keeping a collection beside
 * the main state there too — `pinStatuses` is separate for the same reason.
 */
@HiltViewModel
class CastDevicesViewModel @Inject constructor(
	private val store: CastDeviceStore,
	private val probe: CastProbe,
) : ViewModel() {

	val devices: StateFlow<List<ManualCastDevice>> = store.devices
		.stateIn(viewModelScope, SharingStarted.WhileSubscribed(5_000), emptyList())

	private val _draft = MutableStateFlow<CastDeviceDraft?>(null)
	val draft: StateFlow<CastDeviceDraft?> = _draft.asStateFlow()

	fun add() {
		_draft.value = CastDeviceDraft()
	}

	fun edit(device: ManualCastDevice) {
		_draft.value = CastDeviceDraft(
			id = device.id,
			address = device.address,
			name = device.name,
			port = device.port.toString(),
		)
	}

	fun dismiss() {
		_draft.value = null
	}

	// Each setter clears the last test result: it was about an address that is
	// no longer what the field says, and a stale verdict on screen is worse
	// than none.
	fun onAddress(v: String) = _draft.update { it?.copy(address = v, testResult = null) }

	fun onPort(v: String) = _draft.update { it?.copy(port = v, testResult = null) }

	/**
	 * Alone in not clearing the result — the name is a label we attach, not part
	 * of what was asked of the network.
	 */
	fun onName(v: String) = _draft.update { it?.copy(name = v) }

	fun test() {
		val current = _draft.value ?: return
		val port = current.portOrNull ?: return
		_draft.update { it?.copy(testing = true, testResult = null) }
		viewModelScope.launch {
			val target = ManualCastDevice(
				id = current.id ?: "probe",
				address = current.address.trim(),
				name = current.name,
				port = port,
			).toCastDevice()
			val result = probe.probe(target)
			_draft.update { draft ->
				// The dialog may have been dismissed, or the address retyped,
				// while the probe was in flight; a result for something else
				// must not land.
				if (draft == null || draft.address.trim() != target.address) return@update draft
				draft.copy(
					testing = false,
					testResult = result,
					// A learned name fills an empty field and never overwrites
					// one the user typed.
					name = draft.name.ifBlank {
						(result as? CastProbeResult.Answered)?.name.orEmpty()
					},
				)
			}
		}
	}

	fun save() {
		val current = _draft.value ?: return
		val port = current.portOrNull ?: return
		val address = current.address.trim()
		val name = current.name.trim()
		viewModelScope.launch {
			if (current.id == null) {
				store.add(ManualCastDevice.create(address, name, port))
			} else {
				store.update(
					ManualCastDevice(id = current.id, address = address, name = name, port = port),
				)
			}
			_draft.value = null
		}
	}

	fun remove(id: String) {
		viewModelScope.launch { store.remove(id) }
	}
}
