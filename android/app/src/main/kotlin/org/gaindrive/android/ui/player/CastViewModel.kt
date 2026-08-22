package org.gaindrive.android.ui.player

import androidx.lifecycle.ViewModel
import androidx.lifecycle.viewModelScope
import dagger.hilt.android.lifecycle.HiltViewModel
import kotlinx.coroutines.flow.SharingStarted
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.combine
import kotlinx.coroutines.flow.stateIn
import org.gaindrive.android.data.CastDeviceStore
import org.gaindrive.android.data.toCastDevice
import org.gaindrive.android.playback.cast.CastDevice
import org.gaindrive.android.playback.cast.CastDiscovery
import org.gaindrive.android.playback.cast.CastSession
import javax.inject.Inject

/**
 * The device picker's view of casting: what is on the network, what the user
 * named by hand, and what we are connected to.
 *
 * All three collaborators are singletons — discovery and the control channel
 * outlive any screen — so this is nearly a pass-through, like [PlayerViewModel].
 */
@HiltViewModel
class CastViewModel @Inject constructor(
	private val discovery: CastDiscovery,
	private val session: CastSession,
	store: CastDeviceStore,
) : ViewModel() {

	val devices: StateFlow<List<CastDevice>> = discovery.devices
	val connected: StateFlow<CastDevice?> = session.device

	/**
	 * Manually added devices, minus any that discovery has also found.
	 *
	 * Deduplicated by address and resolved in discovery's favour, because its
	 * entry carries the friendly name from the `fn` TXT record and the stable
	 * `id`, where a manual entry has only what someone typed. So a device that
	 * starts announcing itself again quietly stops appearing twice, with no
	 * housekeeping asked of the user.
	 *
	 * Merged here rather than into [CastDiscovery] on purpose: its `publish()`
	 * replaces the whole device list from its own map on every announcement, so
	 * anything injected there would be erased by the next one.
	 */
	val manualDevices: StateFlow<List<CastDevice>> =
		combine(store.devices, discovery.devices) { manual, found ->
			val seen = found.mapTo(mutableSetOf()) { it.address }
			manual.filterNot { it.address in seen }.map { it.toCastDevice() }
		}.stateIn(viewModelScope, SharingStarted.WhileSubscribed(5_000), emptyList())

	/**
	 * Discovery is tied to the picker being open rather than to this view model's
	 * lifetime: it holds a multicast conversation open, and nothing off-screen
	 * needs to know what is on the network.
	 */
	fun startDiscovery() = discovery.start()

	fun stopDiscovery() = discovery.stop()

	fun select(device: CastDevice) = session.connect(device)

	fun disconnect() = session.disconnect()
}
