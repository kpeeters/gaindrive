package org.gaindrive.android.playback.cast

import android.annotation.SuppressLint
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.sync.Mutex
import kotlinx.coroutines.sync.withLock
import kotlinx.coroutines.withContext
import kotlinx.serialization.json.Json
import kotlinx.serialization.json.JsonObject
import java.io.Closeable
import java.net.InetSocketAddress
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
 * simplest thing there. Here a single connection is multiplexed — which is what
 * pychromecast and node-castv2 do — so commands cost no handshake and cannot
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
	 * A timeout waiting for the *first* byte is ordinary quiet — the receiver
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
		private const val FIRST_TIMEOUT = -1
		private const val FIRST_CLOSED = -2

		private const val CONNECT_TIMEOUT_MS = 5_000

		/**
		 * Short enough that the receive loop wakes regularly to poll for
		 * position and to notice it should stop; the receiver only pushes
		 * `MEDIA_STATUS` on state changes, never during steady playback.
		 */
		const val READ_TIMEOUT_MS = 1_000

		suspend fun open(device: CastDevice, json: Json): CastChannel? =
			withContext(Dispatchers.IO) {
				runCatching {
					val socket = context().socketFactory.createSocket() as SSLSocket
					socket.connect(
						InetSocketAddress(device.address, device.port),
						CONNECT_TIMEOUT_MS,
					)
					socket.soTimeout = READ_TIMEOUT_MS
					socket.startHandshake()
					CastChannel(socket, json)
				}.getOrNull()
			}

		/**
		 * A permissive trust manager, scoped to this socket factory and never
		 * installed as a default.
		 *
		 * Chromecasts present device certificates chaining to a Google root that
		 * is not in the Android trust store, so ordinary verification cannot
		 * succeed — the C++ uses `SSL_VERIFY_NONE` for the same reason. What
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
