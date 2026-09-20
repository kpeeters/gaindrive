package org.gaindrive.android.playback.cast

import android.util.Log
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.currentCoroutineContext
import kotlinx.coroutines.isActive
import kotlinx.coroutines.withContext
import kotlinx.serialization.json.Json
import kotlinx.serialization.json.JsonArray
import kotlinx.serialization.json.JsonObject
import kotlinx.serialization.json.JsonPrimitive
import kotlinx.serialization.json.buildJsonObject
import kotlinx.serialization.json.put
import okhttp3.OkHttpClient
import okhttp3.Request
import java.util.concurrent.TimeUnit
import java.util.concurrent.atomic.AtomicInteger
import javax.inject.Inject
import javax.inject.Singleton

/** What asking a device whether it is there produced. */
sealed interface CastProbeResult {
	/**
	 * It spoke Cast. [runningApp] is whatever the television is showing right
	 * now — often its own ambient app rather than anything of ours — and [name]
	 * is set only when the best-effort lookup below happened to work.
	 */
	data class Answered(val runningApp: String?, val name: String?) : CastProbeResult

	/**
	 * Something accepted a TLS connection on the cast port and then said
	 * nothing. Kept distinct from [Unreachable] because it is different advice:
	 * the address is live and reachable, but whatever is there is not a
	 * Chromecast.
	 */
	data object Silent : CastProbeResult

	/** No control channel at all — wrong address, device off, or blocked. */
	data object Unreachable : CastProbeResult
}

/**
 * Asks one device whether it is there, without disturbing anything.
 *
 * [CastSession.connect] cannot serve as a test button. It is a singleton that
 * publishes the target it was given, and `PlaybackService.watchCastDevice()`
 * reacts to that by handing the queue to a [CastPlayer] and swapping the
 * `MediaSession`'s player — so testing an address the user has merely typed
 * would start casting to it. This runs the same opening exchange over its own
 * channel and throws the channel away.
 *
 * It lives in this package because [CastChannel], [CastNs] and [CastRx] are
 * `internal` to it.
 */
@Singleton
class CastProbe @Inject constructor(
	private val json: Json,
	private val wifi: WifiNetworks,
) {

	private val requestIds = AtomicInteger(1)

	/**
	 * Connect, ask for a receiver status, report what came back.
	 *
	 * This exercises TCP reachability, the TLS handshake and the Cast framing,
	 * which are the three things a wrong address fails at, in that order.
	 */
	suspend fun probe(device: CastDevice): CastProbeResult = withContext(Dispatchers.IO) {
		val open = CastChannel.open(device, json, wifi)
		if (open == null) {
			Log.i(TAG, "probe ${device.address}: no channel (${wifi.describe()})")
			return@withContext CastProbeResult.Unreachable
		}

		open.use { channel ->
			// The virtual connection to the platform must exist before anything
			// else is accepted — the same order CastSession.runLoop uses.
			channel.send(CastNs.CONNECTION, CastNs.RECEIVER_ID, connectPayload())
			channel.send(CastNs.RECEIVER, CastNs.RECEIVER_ID, request("GET_STATUS"))

			val status = awaitReceiverStatus(channel)
			if (status == null) {
				Log.i(TAG, "probe ${device.address}: connected but silent")
				return@withContext CastProbeResult.Silent
			}

			Log.i(TAG, "probe ${device.address}: RECEIVER_STATUS $status")
			CastProbeResult.Answered(
				runningApp = displayNameOf(status),
				name = friendlyName(device.address),
			)
		}
	}

	/**
	 * Reads until the receiver answers, keeping the heartbeat alive meanwhile.
	 * Null means it never did.
	 *
	 * The deadline is checked here rather than by wrapping the loop in
	 * `withTimeoutOrNull`, and that is not a style choice.
	 * [CastChannel.receive] *blocks* for up to `READ_TIMEOUT_MS` instead of
	 * suspending, so a silent device produces a loop with no suspension point in
	 * it at all — and cancellation, which is all a timeout has to work with,
	 * would never be observed. The wrapped version hangs for as long as the
	 * device stays quiet, which is precisely the case this function exists for.
	 *
	 * [isActive] is likewise a plain non-suspending read, so a dismissed dialog
	 * still stops the loop.
	 */
	private suspend fun awaitReceiverStatus(channel: CastChannel): JsonObject? {
		val deadline = System.nanoTime() + ANSWER_TIMEOUT_MS * 1_000_000
		while (currentCoroutineContext().isActive && System.nanoTime() < deadline) {
			when (val rx = channel.receive()) {
				CastRx.Closed -> return null
				CastRx.Idle -> Unit
				is CastRx.Message -> when (CastStatus.typeOf(rx.json)) {
					"RECEIVER_STATUS" -> return rx.json
					// A probe is short enough that a missed PONG would not drop
					// the connection, but answering costs one write and keeps
					// this identical to what the session does.
					"PING" -> channel.send(
						CastNs.HEARTBEAT,
						CastNs.RECEIVER_ID,
						buildJsonObject { put("type", "PONG") },
					)

					else -> Unit
				}
			}
		}
		return null
	}

	/**
	 * The device's friendly name, if it will tell us — best effort, and often
	 * not.
	 *
	 * A `RECEIVER_STATUS` carries no name: discovery gets it from the mDNS `fn`
	 * TXT record, which is precisely the channel that has failed whenever this
	 * feature is being used. `eureka_info` is unauthenticated on older firmware
	 * and locked down on newer Google TV devices, so a miss here is ordinary and
	 * must never turn a successful probe into a failed one.
	 */
	private fun friendlyName(address: String): String? {
		val client = nameClient()
		val request = Request.Builder()
			.url("http://$address:$EUREKA_PORT/setup/eureka_info?options=detail")
			.build()
		return runCatching {
			client.newCall(request).execute().use { response ->
				if (!response.isSuccessful) return@use null
				val body = response.body.string()
				val parsed = json.parseToJsonElement(body) as? JsonObject ?: return@use null
				parsed.string("name")
			}
		}.getOrNull()
	}

	/**
	 * A bare client, built here rather than injected.
	 *
	 * The shared client carries `AuthInterceptor`, which appends the account's
	 * `u=`/`t=`/`s=` parameters to every request. Those are credentials for the
	 * music server and must never be sent to a device that is not it. Bound to
	 * Wi-Fi when there is a Wi-Fi network, for the reason `CastChannel` prefers
	 * that socket: under a full tunnel the LAN is not where the default route
	 * goes.
	 */
	private fun nameClient(): OkHttpClient {
		val builder = OkHttpClient.Builder()
			.connectTimeout(NAME_TIMEOUT_MS, TimeUnit.MILLISECONDS)
			.readTimeout(NAME_TIMEOUT_MS, TimeUnit.MILLISECONDS)
		wifi.network.value?.let { builder.socketFactory(it.socketFactory) }
		return builder.build()
	}

	/**
	 * What the television is showing. Unlike [CastStatus.transportIdOf] this
	 * deliberately takes the *first* application rather than matching on our own
	 * app id: an idle receiver is running its ambient app, and naming that is
	 * more informative to someone testing an address than reporting nothing.
	 */
	private fun displayNameOf(message: JsonObject): String? =
		(message["status"] as? JsonObject)
			?.let { it["applications"] as? JsonArray }
			?.filterIsInstance<JsonObject>()
			?.firstNotNullOfOrNull { it.string("displayName") }

	private fun request(type: String): JsonObject = buildJsonObject {
		put("type", type)
		put("requestId", requestIds.getAndIncrement())
	}

	private fun connectPayload(): JsonObject = buildJsonObject { put("type", "CONNECT") }

	/** Safe by construction, like the rest of the Cast JSON reading. */
	private fun JsonObject.string(key: String): String? =
		(this[key] as? JsonPrimitive)?.takeIf { it.isString }?.content?.takeIf { it.isNotEmpty() }

	private companion object {
		const val TAG = "GainDriveCast"

		/**
		 * Generous: the channel's own connect can already have spent several
		 * seconds, and a receiver waking from standby answers slowly.
		 */
		const val ANSWER_TIMEOUT_MS = 6_000L

		/** The name is a nicety; nobody should wait for it. */
		const val NAME_TIMEOUT_MS = 2_000L

		/** Chromecast's unauthenticated setup endpoint, where it still exists. */
		const val EUREKA_PORT = 8008
	}
}
