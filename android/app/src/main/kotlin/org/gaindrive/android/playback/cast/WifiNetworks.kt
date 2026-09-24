package org.gaindrive.android.playback.cast

import android.content.Context
import android.net.ConnectivityManager
import android.net.LinkAddress
import android.net.Network
import android.net.NetworkCapabilities
import android.net.NetworkRequest
import dagger.hilt.android.qualifiers.ApplicationContext
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.asStateFlow
import java.net.Inet4Address
import java.net.InetAddress
import javax.inject.Inject
import javax.inject.Singleton

/**
 * The Wi-Fi network, as distinct from whichever network holds the default route.
 *
 * Casting needs this distinction and nothing else in the app does. A Chromecast
 * is only ever reachable over the LAN, so both the reachability probe and the
 * bridge have to talk on Wi-Fi specifically - which, under a full-tunnel VPN, is
 * not where traffic goes by default.
 */
@Singleton
class WifiNetworks @Inject constructor(
	@ApplicationContext context: Context,
) {

	private val manager = context.getSystemService(ConnectivityManager::class.java)

	private val _network = MutableStateFlow<Network?>(null)

	/** Changes identity when the phone joins a different Wi-Fi network. */
	val network: StateFlow<Network?> = _network.asStateFlow()

	init {
		val request = NetworkRequest.Builder()
			.addTransportType(NetworkCapabilities.TRANSPORT_WIFI)
			.build()
		val callback = object : ConnectivityManager.NetworkCallback() {
			override fun onAvailable(network: Network) {
				_network.value = network
			}

			override fun onLost(network: Network) {
				if (_network.value == network) _network.value = null
			}
		}
		// Registration can fail on a device with connectivity restricted. Casting
		// then simply reports no Wi-Fi, which is the truthful answer.
		runCatching { manager?.registerNetworkCallback(request, callback) }
	}

	/**
	 * The phone's own IPv4 address on the Wi-Fi network - what a receiver has to
	 * be given to fetch from the bridge. IPv6 is skipped deliberately: a literal
	 * v6 address in a URL is a bracketed nuisance and every Cast receiver has a
	 * v4 address on the same LAN.
	 */
	fun localAddress(): Inet4Address? = wifiLinkAddress()?.address as? Inet4Address

	/**
	 * What a cast socket failure has to be read against: which Wi-Fi network was
	 * tracked, if any, and the address it would have gone out from.
	 *
	 * Without it, "no Wi-Fi network so we never bound at all", "bound and the
	 * kernel refused the route" and "bound and the receiver did not answer" all
	 * reach logcat as the same failure, which is how the VPN routing bug stayed
	 * invisible for as long as it did.
	 */
	fun describe(): String {
		val net = _network.value ?: return "no Wi-Fi network"
		return "Wi-Fi ${net.networkHandle} at ${localAddress()?.hostAddress ?: "no address"}"
	}

	/**
	 * True when [address] is on the same Wi-Fi subnet as this phone, which is the
	 * bridge's admission test. Anything else has no business fetching a private
	 * music library.
	 */
	fun isOnLocalSubnet(address: InetAddress): Boolean {
		val link = wifiLinkAddress() ?: return false
		val local = link.address as? Inet4Address ?: return false
		val candidate = address as? Inet4Address ?: return false
		return samePrefix(local, candidate, link.prefixLength)
	}

	private fun wifiLinkAddress(): LinkAddress? {
		val net = _network.value ?: return null
		val properties = manager?.getLinkProperties(net) ?: return null
		return properties.linkAddresses.firstOrNull {
			it.address is Inet4Address && !it.address.isLoopbackAddress
		}
	}

	private fun samePrefix(a: Inet4Address, b: Inet4Address, prefixLength: Int): Boolean {
		if (prefixLength !in 0..32) return false
		val left = a.address.fold(0) { acc, byte -> (acc shl 8) or (byte.toInt() and 0xff) }
		val right = b.address.fold(0) { acc, byte -> (acc shl 8) or (byte.toInt() and 0xff) }
		// A /0 would shift by 32, which is a no-op on Int in both Java and Kotlin
		// and would compare nothing at all.
		if (prefixLength == 0) return true
		val mask = (-1 shl (32 - prefixLength))
		return (left and mask) == (right and mask)
	}
}
