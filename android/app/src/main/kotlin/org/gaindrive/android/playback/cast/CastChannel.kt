package org.gaindrive.android.playback.cast

import android.annotation.SuppressLint
import android.util.Log
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.sync.Mutex
import kotlinx.coroutines.sync.withLock
import kotlinx.coroutines.withContext
import kotlinx.serialization.json.Json
import kotlinx.serialization.json.JsonObject
import java.io.Closeable
import java.net.InetSocketAddress
import java.net.Socket
import java.net.SocketTimeoutException
import java.security.SecureRandom
import java.security.cert.X509Certificate
import javax.net.ssl.SSLContext
import javax.net.ssl.SSLSocket
import javax.net.ssl.X509TrustManager

/** The four Cast namespaces this app speaks. */
internal object CastNs {
	const val CONNECTION = "urn:x-cast:com.google.cast.tp.connection"
	const val HEARTBEAT = "urn:x-cast:com.google.cast.tp.heartbeat"
	const val RECEIVER = "urn:x-cast:com.google.cast.receiver"
	const val MEDIA = "urn:x-cast:com.google.cast.media"

	/** Every message we send comes from this id; the receiver echoes it back. */
	const val SENDER = "sender-0"

	/** The receiver platform itself, as opposed to an app's transport. */
	const val RECEIVER_ID = "receiver-0"

	/** The Default Media Receiver. */
	const val DEFAULT_MEDIA_APP = "CC1AD845"
}

/** One turn of the receive loop. */
internal sealed interface CastRx {
	data class Message(val json: JsonObject) : CastRx

	/**
	 * The read timeout expired with nothing pending, or a frame arrived that
	 * carried nothing we could read. Either way there is no news, and the caller
	 * gets its chance to poll and to re-check whether it should still be running.
	 */
	data object Idle : CastRx

	/** The connection is gone, or is out of step and cannot be resynced. */
	data object Closed : CastRx
}

/**
 * A TLS connection to a Chromecast's control port, framing Cast messages in
 * both directions.
 *
 * Ported from the `Tls` struct and `cast_send`/`cast_recv` in
 * `src/castmanager.cc`, with one deliberate change: the C++ opens a fresh
 * connection for every command because a detached thread per command was the
 * simplest thing there. Here a single connection is multiplexed - which is what
 * pychromecast and node-castv2 do - so commands cost no handshake and cannot
 * race a reconnect. [send] is serialised; reads happen only on the session's
 * receive loop.
 */
internal class CastChannel private constructor(
	private val socket: SSLSocket,
	private val json: Json,
) : Closeable {

	private val input = socket.inputStream
	private val output = socket.outputStream
	private val writeLock = Mutex()

	suspend fun send(
		namespace: String,
		destination: String,
		payload: JsonObject,
	): Boolean = withContext(Dispatchers.IO) {
		val bytes = CastMessage.frame(
			CastMessage.body(namespace, CastNs.SENDER, destination, payload.toString())
		)
		writeLock.withLock {
			runCatching {
				output.write(bytes)
				output.flush()
			}.isSuccess
		}
	}

	/**
	 * Blocking read of one message. Call from an IO dispatcher.
	 *
	 * A timeout waiting for the *first* byte is ordinary quiet - the receiver
	 * only pushes on state changes. A timeout part-way through a frame is not
	 * recoverable: the stream is then out of step with the length prefix and
	 * there is no way to resync, so it is reported as [CastRx.Closed] and the
	 * caller reconnects.
	 */
	fun receive(): CastRx {
		val header = ByteArray(4)
		when (val first = readFirstByte()) {
			FIRST_TIMEOUT -> return CastRx.Idle
			FIRST_CLOSED -> return CastRx.Closed
			else -> header[0] = first.toByte()
		}
		if (!readFully(header, 1)) return CastRx.Closed

		val length = CastMessage.frameLength(header)
		if (length <= 0 || length > CastMessage.MAX_FRAME) return CastRx.Closed

		val body = ByteArray(length)
		if (!readFully(body, 0)) return CastRx.Closed

		val payload = CastMessage.payloadOf(body) ?: return CastRx.Idle
		val parsed = runCatching { json.parseToJsonElement(payload) as? JsonObject }.getOrNull()
		return if (parsed == null) CastRx.Idle else CastRx.Message(parsed)
	}

	private fun readFirstByte(): Int = try {
		val b = input.read()
		if (b < 0) FIRST_CLOSED else b
	} catch (_: SocketTimeoutException) {
		FIRST_TIMEOUT
	} catch (_: Exception) {
		FIRST_CLOSED
	}

	private fun readFully(into: ByteArray, from: Int): Boolean {
		var got = from
		while (got < into.size) {
			val n = try {
				input.read(into, got, into.size - got)
			} catch (_: Exception) {
				return false
			}
			if (n <= 0) return false
			got += n
		}
		return true
	}

	override fun close() {
		runCatching { socket.close() }
	}

	companion object {
		private const val TAG = "GainDriveCast"

		private const val FIRST_TIMEOUT = -1
		private const val FIRST_CLOSED = -2

		private const val CONNECT_TIMEOUT_MS = 5_000

		/**
		 * The fallback only runs once the first attempt has already spent its own
		 * timeout, so it is kept shorter: a receiver that is simply switched off
		 * then costs eight seconds a round rather than ten.
		 */
		private const val FALLBACK_TIMEOUT_MS = 3_000

		/**
		 * Short enough that the receive loop wakes regularly to poll for
		 * position and to notice it should stop; the receiver only pushes
		 * `MEDIA_STATUS` on state changes, never during steady playback.
		 */
		const val READ_TIMEOUT_MS = 1_000

		/**
		 * Opens the control channel, layering TLS over a plain socket from
		 * [connectPlain] because `SSLSocketFactory` has no notion of a network to
		 * bind to.
		 *
		 * Note the asymmetry, which is deliberate and matches [CastBridge]: this
		 * channel and the reachability probe reach for the Wi-Fi network, while
		 * fetches from the music server stay on the default route - that is how
		 * the phone reaches a server over the VPN and a receiver over the LAN at
		 * once.
		 */
		suspend fun open(device: CastDevice, json: Json, wifi: WifiNetworks): CastChannel? =
			withContext(Dispatchers.IO) {
				val plain = connectPlain(device, wifi) ?: return@withContext null
				runCatching {
					val socket = context().socketFactory.createSocket(
						plain,
						device.address,
						device.port,
						/* autoClose = */ true,
					) as SSLSocket
					// The handshake reads on this socket, so it cannot run under
					// the steady-state timeout: a certificate exchange that pauses
					// for a second is ordinary, and would otherwise abort the
					// connection and be reported as one more failure to connect.
					socket.soTimeout = CONNECT_TIMEOUT_MS
					socket.startHandshake()
					socket.soTimeout = READ_TIMEOUT_MS
					CastChannel(socket, json)
				}.onFailure {
					Log.w(TAG, "cast TLS handshake with ${device.address} failed", it)
					runCatching { plain.close() }
				}.getOrNull()
			}

		/**
		 * A TCP connection to the receiver, over the Wi-Fi network if the kernel
		 * permits it and over the default route if it does not.
		 *
		 * Binding to the Wi-Fi network is the right thing under a full tunnel,
		 * where everything addressed to the LAN over the default route goes into
		 * the tunnel and dies. But it is *refused outright* while an ordinary
		 * `VpnService` is up. A VPN that has not called `allowBypass()` - and
		 * WireGuard does not - makes every other network off limits to the apps
		 * it covers, and the refusal comes from `Network.bindSocket` inside
		 * `createSocket()`:
		 *
		 *     java.net.SocketException: Binding socket to network 1084 failed:
		 *     EPERM (Operation not permitted)
		 *
		 * So no route is consulted and no connection is attempted - which is why
		 * this looked like an unreachable receiver and cost a long investigation.
		 * An *unbound* socket is not covered by that restriction and reaches a
		 * LAN address perfectly well, which is why the fallback works where the
		 * binding does not, and why [CastBridge] - whose `ServerSocket` is bound
		 * to an address rather than to a network - could serve the LAN all along
		 * while this channel could not reach it.
		 *
		 * Neither order suits every configuration, so both are tried. Wi-Fi goes
		 * first because its failure is instant, while an unbound attempt swallowed
		 * by a full tunnel has to time out.
		 */
		private fun connectPlain(device: CastDevice, wifi: WifiNetworks): Socket? {
			val address = InetSocketAddress(device.address, device.port)
			val network = wifi.network.value
			val where = wifi.describe()

			// No Wi-Fi network is no reason to fail outright, and no reason to
			// spend a timeout finding that out: go straight to the default route,
			// which without a VPN reaches the LAN perfectly well.
			if (network != null) {
				attempt("Wi-Fi-bound", device, where) {
					network.socketFactory.createSocket()
						.connectOrClose(address, CONNECT_TIMEOUT_MS)
				}?.let { return it }
			}

			return attempt("unbound", device, where) {
				Socket().connectOrClose(address, FALLBACK_TIMEOUT_MS)
			}
		}

		private fun attempt(
			how: String,
			device: CastDevice,
			where: String,
			create: () -> Socket,
		): Socket? =
			runCatching(create)
				.onSuccess { Log.i(TAG, "cast connected to ${device.address} $how ($where)") }
				// The message, not the throwable: for a socket failure it carries
				// the errno, which is the whole of the diagnosis, and the stack is
				// sixteen frames of coroutine plumbing under R8 names.
				.onFailure { Log.w(TAG, "cast connect to ${device.address} $how failed ($where): $it") }
				.getOrNull()

		/** Connects, or closes, so that a failed attempt leaves no socket behind. */
		private fun Socket.connectOrClose(address: InetSocketAddress, timeout: Int): Socket {
			try {
				connect(address, timeout)
			} catch (e: Exception) {
				runCatching { close() }
				throw e
			}
			return this
		}

		/**
		 * A permissive trust manager, scoped to this socket factory and never
		 * installed as a default.
		 *
		 * Chromecasts present device certificates chaining to a Google root that
		 * is not in the Android trust store, so ordinary verification cannot
		 * succeed - the C++ uses `SSL_VERIFY_NONE` for the same reason. What
		 * authenticates the exchange is not the certificate: the user picked this
		 * device off their own network, and the stream URL carries a per-session
		 * token. Nothing else in the app goes near this factory.
		 */
		@SuppressLint("CustomX509TrustManager", "TrustAllX509TrustManager")
		private fun context(): SSLContext {
			val trustAny = object : X509TrustManager {
				override fun checkClientTrusted(chain: Array<X509Certificate>, authType: String) = Unit
				override fun checkServerTrusted(chain: Array<X509Certificate>, authType: String) = Unit
				override fun getAcceptedIssuers(): Array<X509Certificate> = emptyArray()
			}
			return SSLContext.getInstance("TLS").apply {
				init(null, arrayOf(trustAny), SecureRandom())
			}
		}
	}
}
