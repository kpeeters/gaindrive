package org.gaindrive.android.playback.cast

import android.content.Context
import android.net.nsd.NsdManager
import android.net.nsd.NsdServiceInfo
import android.util.Log
import dagger.hilt.android.qualifiers.ApplicationContext
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Job
import kotlinx.coroutines.channels.Channel
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.asStateFlow
import kotlinx.coroutines.isActive
import kotlinx.coroutines.launch
import kotlinx.coroutines.suspendCancellableCoroutine
import kotlinx.coroutines.withTimeoutOrNull
import java.util.concurrent.ConcurrentHashMap
import javax.inject.Inject
import javax.inject.Singleton
import kotlin.coroutines.resume

/**
 * Finds Chromecasts on the local network with `NsdManager`.
 *
 * `NsdManager` replaces roughly 270 lines of mDNS in `src/castmanager.cc` and
 * needs no permission, which is why it is tried first. It also has a long
 * reputation for flakiness across OEM builds; jmDNS is the documented fallback
 * (`CAST.md`), and it is deliberately not here yet, because it brings its own
 * Chromecast bug — a device that reboots is never re-announced — and a
 * `MulticastLock` besides. Add it if a real device proves it necessary, not
 * before.
 *
 * Discovery runs only while something is watching: it holds a multicast
 * conversation open, and there is no reason to pay for that on every screen.
 */
@Singleton
class CastDiscovery @Inject constructor(
	@ApplicationContext context: Context,
	private val scope: CoroutineScope,
) {

	private val nsd: NsdManager? = context.getSystemService(NsdManager::class.java)

	private val _devices = MutableStateFlow<List<CastDevice>>(emptyList())
	val devices: StateFlow<List<CastDevice>> = _devices.asStateFlow()

	/** Keyed by mDNS service name, which is what `onServiceLost` reports. */
	private val found = ConcurrentHashMap<String, CastDevice>()

	private var listener: NsdManager.DiscoveryListener? = null
	private var resolver: Job? = null

	/**
	 * Resolves are queued and performed one at a time. Concurrent
	 * `resolveService` calls are the best-known way to make `NsdManager` fail
	 * with "listener already in use", and a burst of announcements at startup is
	 * exactly when that would happen.
	 */
	private var pending: Channel<NsdServiceInfo>? = null

	@Synchronized
	fun start() {
		val manager = nsd ?: return
		if (listener != null) return

		val queue = Channel<NsdServiceInfo>(Channel.UNLIMITED)
		pending = queue
		resolver = scope.launch { resolveLoop(queue) }

		val active = object : NsdManager.DiscoveryListener {
			override fun onDiscoveryStarted(serviceType: String) {
				Log.i(TAG, "discovery started")
			}

			override fun onServiceFound(serviceInfo: NsdServiceInfo) {
				queue.trySend(serviceInfo)
			}

			override fun onServiceLost(serviceInfo: NsdServiceInfo) {
				if (found.remove(serviceInfo.serviceName) != null) publish()
			}

			override fun onDiscoveryStopped(serviceType: String) {
				Log.i(TAG, "discovery stopped")
			}

			override fun onStartDiscoveryFailed(serviceType: String, errorCode: Int) {
				Log.w(TAG, "discovery failed to start: $errorCode")
			}

			override fun onStopDiscoveryFailed(serviceType: String, errorCode: Int) {
				Log.w(TAG, "discovery failed to stop: $errorCode")
			}
		}
		listener = active
		runCatching {
			manager.discoverServices(SERVICE_TYPE, NsdManager.PROTOCOL_DNS_SD, active)
		}.onFailure {
			Log.w(TAG, "discoverServices threw: ${it.message}")
			listener = null
		}
	}

	@Synchronized
	fun stop() {
		val manager = nsd
		listener?.let { active ->
			// Throws if discovery already stopped itself; that is not worth
			// crashing over, and there is nothing to do about it either way.
			runCatching { manager?.stopServiceDiscovery(active) }
		}
		listener = null
		pending?.close()
		pending = null
		resolver?.cancel()
		resolver = null
		// The list is deliberately kept: reopening the picker should show what
		// was there a moment ago rather than an empty box that fills in.
	}

	private suspend fun resolveLoop(queue: Channel<NsdServiceInfo>) {
		for (info in queue) {
			if (!scope.isActive) return
			val resolved = withTimeoutOrNull(RESOLVE_TIMEOUT_MS) { resolve(info) } ?: continue
			val device = resolved.toCastDevice() ?: continue
			found[resolved.serviceName] = device
			publish()
		}
	}

	@Suppress("DEPRECATION")
	private suspend fun resolve(info: NsdServiceInfo): NsdServiceInfo? =
		suspendCancellableCoroutine { continuation ->
			val manager = nsd
			if (manager == null) {
				continuation.resume(null)
				return@suspendCancellableCoroutine
			}
			// resolveService is deprecated from API 34 in favour of
			// registerServiceInfoCallback, which does not exist on the API 26
			// floor this app supports. One code path beats two until the
			// deprecated one actually breaks.
			manager.resolveService(info, object : NsdManager.ResolveListener {
				override fun onResolveFailed(serviceInfo: NsdServiceInfo, errorCode: Int) {
					Log.w(TAG, "resolve failed for ${serviceInfo.serviceName}: $errorCode")
					if (continuation.isActive) continuation.resume(null)
				}

				override fun onServiceResolved(serviceInfo: NsdServiceInfo) {
					if (continuation.isActive) continuation.resume(serviceInfo)
				}
			})
		}

	private fun publish() {
		_devices.value = found.values.sortedBy { it.name.lowercase() }
	}

	@Suppress("DEPRECATION")
	private fun NsdServiceInfo.toCastDevice(): CastDevice? {
		val address = host?.hostAddress ?: return null
		// `fn` is the friendly name the user set on the device; the service name
		// is a serial-number-ish string nobody would recognise. `id` is stable
		// across a rename, which the service name is not.
		val text = attributes.orEmpty()
		val friendly = text["fn"]?.toString(Charsets.UTF_8)?.takeIf { it.isNotBlank() }
		val id = text["id"]?.toString(Charsets.UTF_8)?.takeIf { it.isNotBlank() }
		return CastDevice(
			id = id ?: serviceName,
			name = friendly ?: serviceName,
			address = address,
			port = if (port > 0) port else 8009,
		)
	}

	private companion object {
		const val TAG = "GainDriveCast"
		const val SERVICE_TYPE = "_googlecast._tcp."
		const val RESOLVE_TIMEOUT_MS = 5_000L
	}
}
