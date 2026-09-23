package org.gaindrive.android.net

import android.content.Context
import android.net.ConnectivityManager
import android.net.LinkAddress
import dagger.hilt.android.qualifiers.ApplicationContext
import java.net.Inet4Address
import java.net.InetAddress
import java.net.NetworkInterface
import javax.inject.Inject
import javax.inject.Singleton

/**
 * The device's own LAN IPv4, whatever carries it.
 *
 * Not [org.gaindrive.android.playback.cast.WifiNetworks]: that class is
 * Wi-Fi-specific on purpose - casting must go out over Wi-Fi even under a
 * VPN, and CastChannel, CastProbe and CastBridge lean on that - while
 * pairing runs on a television that is as likely to be on Ethernet. The
 * default network is asked first; the interface scan is the fallback for a
 * box whose ConnectivityManager answers oddly.
 */
@Singleton
class LanAddress @Inject constructor(
	@ApplicationContext context: Context,
) {

	private val manager = context.getSystemService(ConnectivityManager::class.java)

	fun localAddress(): Inet4Address? =
		defaultLinkAddress()?.address as? Inet4Address ?: scannedAddress()

	/**
	 * True when [address] shares this device's subnet, the same admission test
	 * the cast bridge applies: nothing beyond the LAN has business here.
	 */
	fun isOnLocalSubnet(address: InetAddress): Boolean {
		val candidate = address as? Inet4Address ?: return false
		defaultLinkAddress()?.let { link ->
			val local = link.address as? Inet4Address ?: return false
			return samePrefix(local, candidate, link.prefixLength)
		}
		val fallback = scannedInterfaceAddress() ?: return false
		val local = fallback.address as? Inet4Address ?: return false
		return samePrefix(local, candidate, fallback.networkPrefixLength.toInt())
	}

	private fun defaultLinkAddress(): LinkAddress? {
		val net = manager?.activeNetwork ?: return null
		val properties = manager.getLinkProperties(net) ?: return null
		return properties.linkAddresses.firstOrNull {
			it.address is Inet4Address && !it.address.isLoopbackAddress
		}
	}

	private fun scannedAddress(): Inet4Address? =
		scannedInterfaceAddress()?.address as? Inet4Address

	private fun scannedInterfaceAddress(): java.net.InterfaceAddress? = runCatching {
		NetworkInterface.getNetworkInterfaces().asSequence()
			.filter { it.isUp && !it.isLoopback }
			.flatMap { it.interfaceAddresses.asSequence() }
			.firstOrNull { address ->
				(address.address as? Inet4Address)?.isSiteLocalAddress == true
			}
	}.getOrNull()

	// The same arithmetic WifiNetworks keeps private; duplicated rather than
	// shared, because tying pairing to the cast package for ten lines would
	// couple the wrong things.
	private fun samePrefix(a: Inet4Address, b: Inet4Address, prefixLength: Int): Boolean {
		if (prefixLength !in 0..32) return false
		if (prefixLength == 0) return true
		val left = a.address.fold(0) { acc, byte -> (acc shl 8) or (byte.toInt() and 0xff) }
		val right = b.address.fold(0) { acc, byte -> (acc shl 8) or (byte.toInt() and 0xff) }
		val mask = (-1 shl (32 - prefixLength))
		return (left and mask) == (right and mask)
	}
}
