package org.gaindrive.android.ui.player

import androidx.lifecycle.ViewModel
import dagger.hilt.android.lifecycle.HiltViewModel
import kotlinx.coroutines.flow.StateFlow
import org.gaindrive.android.playback.cast.CastDevice
import org.gaindrive.android.playback.cast.CastDiscovery
import org.gaindrive.android.playback.cast.CastSession
import javax.inject.Inject

/**
 * The device picker's view of casting: what is on the network, and what we are
 * connected to.
 *
 * Both collaborators are singletons — discovery and the control channel outlive
 * any screen — so this is a pass-through, like [PlayerViewModel].
 */
@HiltViewModel
class CastViewModel @Inject constructor(
	private val discovery: CastDiscovery,
	private val session: CastSession,
) : ViewModel() {

	val devices: StateFlow<List<CastDevice>> = discovery.devices
	val connected: StateFlow<CastDevice?> = session.device

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
