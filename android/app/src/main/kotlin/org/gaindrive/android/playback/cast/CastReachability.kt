package org.gaindrive.android.playback.cast

import android.util.Log
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.withContext
import org.gaindrive.android.data.model.ServerConfig
import java.net.InetSocketAddress
import java.net.URI
import java.util.concurrent.ConcurrentHashMap
import javax.inject.Inject
import javax.inject.Singleton

/**
 * Whether a Cast receiver can be expected to fetch from a server itself.
 *
 * The question is asked the way the receiver would ask it: open a socket to the
 * server **bound to the Wi-Fi network** rather than to the default route, which
 * under a VPN is a different thing entirely. If something on this LAN can reach
 * the server, the receiver almost certainly can too.
 *
 * It is not a proof. Client isolation can block the receiver while allowing the
 * phone, and the two can sit on different VLANs. `CAST.md` calls for a backstop
 * — a direct load that fails with a receiver-side network error should demote
 * that server to the bridge for this network — and that is **not wired yet**: it
 * has to be told apart from every other reason a LOAD fails, and it races the
 * protocol's own LOAD retry. It needs a real roaming test to get right.
 *
 * Direct is worth this trouble because it is functionally better, not merely
 * cheaper: once the receiver has a URL it is independent of the phone, so
 * playback survives the phone sleeping, going out of range or running flat.
 */
@Singleton
class CastReachability @Inject constructor(
	private val wifi: WifiNetworks,
) {

	/** Keyed by server *and* network: the answer changes when the phone moves. */
	private val cache = ConcurrentHashMap<String, Boolean>()

	suspend fun canReachDirectly(config: ServerConfig): Boolean {
		// No Wi-Fi means no receiver either, so the answer does not matter; false
		// is the safe one, since the bridge at least reports a clear failure.
		val network = wifi.network.value ?: return false
		val key = "${config.id.value}@${network.networkHandle}"
		cache[key]?.let { return it }

		val reachable = withContext(Dispatchers.IO) { probe(config) }
		cache[key] = reachable
		Log.i(TAG, "probe ${config.name}: ${if (reachable) "direct" else "bridge"}")
		return reachable
	}

	private fun probe(config: ServerConfig): Boolean {
		val network = wifi.network.value ?: return false
		val uri = runCatching { URI(config.url) }.getOrNull() ?: return false
		val host = uri.host ?: return false
		val port = when {
			uri.port > 0 -> uri.port
			uri.scheme.equals("https", ignoreCase = true) -> 443
			else -> 80
		}

		return runCatching {
			// Resolved on the Wi-Fi network too, not just connected over it. A
			// name that only exists in the VPN's DNS must not resolve here — that
			// it cannot is precisely the signal we are looking for.
			val address = network.getAllByName(host).firstOrNull() ?: return false
			network.socketFactory.createSocket().use { socket ->
				socket.connect(InetSocketAddress(address, port), PROBE_TIMEOUT_MS)
				true
			}
		}.getOrDefault(false)
	}

	private companion object {
		const val TAG = "GainDriveCast"

		/** Long enough for a sleepy LAN host, short enough not to stall a tap. */
		const val PROBE_TIMEOUT_MS = 1_500
	}
}
