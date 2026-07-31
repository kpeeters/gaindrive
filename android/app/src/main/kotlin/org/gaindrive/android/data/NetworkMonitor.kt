package org.gaindrive.android.data

import android.content.Context
import android.net.ConnectivityManager
import android.net.Network
import android.net.NetworkCapabilities
import android.net.NetworkRequest
import dagger.hilt.android.qualifiers.ApplicationContext
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.asStateFlow
import javax.inject.Inject
import javax.inject.Singleton

/**
 * Whether the device has a network at all.
 *
 * Deliberately does not ask whether that network reaches the internet.
 * GainDrive servers are commonly self-hosted on a LAN with no route out, so a
 * validated-internet check would call the app offline exactly when the server
 * is closest to hand. Whether a *server* answers is a separate question, and
 * one the repository already reports per server.
 *
 * Starts optimistic: nothing should be dimmed as unavailable in the moment
 * before the first callback arrives.
 */
@Singleton
class NetworkMonitor @Inject constructor(
	@ApplicationContext context: Context,
) {

	private val manager = context.getSystemService(ConnectivityManager::class.java)

	private val _online = MutableStateFlow(true)
	val online: StateFlow<Boolean> = _online.asStateFlow()

	init {
		val request = NetworkRequest.Builder()
			.addCapability(NetworkCapabilities.NET_CAPABILITY_INTERNET)
			.build()
		val callback = object : ConnectivityManager.NetworkCallback() {
			override fun onAvailable(network: Network) = publish()
			override fun onLost(network: Network) = publish()
			override fun onUnavailable() = publish()
		}
		// Registration can fail on a device with connectivity disabled; being
		// stuck optimistic is a better failure than crashing at startup.
		runCatching { manager?.registerNetworkCallback(request, callback) }
		publish()
	}

	private fun publish() {
		_online.value = manager?.activeNetwork != null
	}
}
