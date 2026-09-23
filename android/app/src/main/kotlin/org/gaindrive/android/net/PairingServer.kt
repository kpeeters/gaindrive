package org.gaindrive.android.net

import android.util.Log
import java.io.BufferedOutputStream
import java.io.IOException
import java.net.ServerSocket
import java.net.Socket
import java.security.SecureRandom
import javax.inject.Inject
import javax.inject.Singleton
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.Job
import kotlinx.coroutines.launch
import kotlinx.serialization.json.Json
import org.gaindrive.android.data.PairPayload
import org.gaindrive.android.data.PairServer
import org.gaindrive.android.data.buildPairUri
import org.gaindrive.android.data.crypto.PairingCipher

/**
 * The TV end of QR pairing: a one-shot HTTP listener that exists only while
 * the pairing pane is on screen.
 *
 * Modeled on [org.gaindrive.android.playback.cast.CastBridge], shrunk to one
 * job: accept `POST /<token>`, decrypt the body with the key the QR carried,
 * hand the servers to the callback, answer 204 and die. The admission tests
 * are the bridge's - same subnet only, unguessable token in the path - plus
 * GCM authentication, so a request that was not composed from this QR cannot
 * even be decrypted.
 *
 * Serial by design where the bridge is concurrent: pairing has exactly one
 * legitimate client, and the socket closing is what ends the accept loop.
 */
@Singleton
class PairingServer @Inject constructor(
	private val lan: LanAddress,
	private val json: Json,
	private val scope: CoroutineScope,
) {

	private var server: ServerSocket? = null
	private var acceptJob: Job? = null

	/**
	 * Starts listening and returns the `gaindrive://pair` URI to put in the
	 * QR, or null when there is no LAN address to listen on. [onReceived] is
	 * called on an IO dispatcher with the decrypted servers; the listener has
	 * already shut itself down by then. Calling start while running returns a
	 * fresh session: the old socket, token and key are discarded.
	 */
	@Synchronized
	fun start(onReceived: (List<PairServer>) -> Unit): String? {
		stopLocked()
		val address = lan.localAddress()
		if (address == null) {
			Log.w(TAG, "pairing: no LAN address to listen on")
			return null
		}
		return runCatching {
			val socket = ServerSocket(0, BACKLOG, address)
			val token = newToken()
			val key = PairingCipher.newKey()
			server = socket
			acceptJob = scope.launch(Dispatchers.IO) {
				acceptLoop(socket, token, key, onReceived)
			}
			val uri = buildPairUri(address.hostAddress ?: "", socket.localPort, token, key)
			Log.i(TAG, "pairing listening on ${address.hostAddress}:${socket.localPort}")
			uri
		}.getOrElse {
			Log.w(TAG, "pairing failed to start: ${it.message}")
			server = null
			null
		}
	}

	@Synchronized
	fun stop() = stopLocked()

	private fun stopLocked() {
		acceptJob?.cancel()
		acceptJob = null
		// Closing the socket is what actually ends the loop: cancellation
		// cannot interrupt a blocking accept().
		runCatching { server?.close() }
		server = null
	}

	private fun acceptLoop(
		socket: ServerSocket,
		token: String,
		key: ByteArray,
		onReceived: (List<PairServer>) -> Unit,
	) {
		while (true) {
			val client = try {
				socket.accept()
			} catch (_: IOException) {
				return // closed, which is how stop() and success both end this
			}
			val done = client.use {
				runCatching { handle(it, token, key, onReceived) }.getOrElse { failure ->
					// A phone abandoning its POST is ordinary; anything else
					// is worth a warning. Either way the listener survives it.
					val level = if (failure is IOException) Log.INFO else Log.WARN
					Log.println(level, TAG, "pairing: request failed - ${failure.message}")
					false
				}
			}
			if (done) {
				stop()
				return
			}
		}
	}

	/** True when a payload was accepted and the listener should die. */
	private fun handle(
		client: Socket,
		token: String,
		key: ByteArray,
		onReceived: (List<PairServer>) -> Unit,
	): Boolean {
		val output = BufferedOutputStream(client.getOutputStream())
		val from = client.inetAddress?.hostAddress ?: "?"

		if (!lan.isOnLocalSubnet(client.inetAddress)) {
			Log.w(TAG, "pairing refused $from: not on the local subnet")
			output.respondEmpty(403, "Forbidden")
			return false
		}

		val reader = client.getInputStream().bufferedReader()
		val requestLine = reader.readLine() ?: return false
		val parts = requestLine.split(' ')
		if (parts.size < 2 || parts[0].uppercase() != "POST" || parts[1] != "/$token") {
			Log.w(TAG, "pairing refused $from: ${parts.getOrNull(0)} ${parts.getOrNull(1)}")
			output.respondEmpty(403, "Forbidden")
			return false
		}

		var length = -1
		while (true) {
			val line = reader.readLine() ?: break
			if (line.isEmpty()) break
			val split = line.indexOf(':')
			if (split <= 0) continue
			if (line.substring(0, split).equals("Content-Length", ignoreCase = true)) {
				length = line.substring(split + 1).trim().toIntOrNull() ?: -1
			}
		}
		if (length !in 1..MAX_BODY_BYTES) {
			Log.w(TAG, "pairing refused $from: content length $length")
			output.respondEmpty(400, "Bad Request")
			return false
		}

		// Through the same reader that consumed the headers, or the buffered
		// prefix of the body is lost. The payload is ASCII (base64:base64),
		// so chars and bytes agree.
		val body = CharArray(length)
		var got = 0
		while (got < length) {
			val n = reader.read(body, got, length - got)
			if (n < 0) break
			got += n
		}
		if (got < length) {
			Log.i(TAG, "pairing: $from closed mid-body")
			return false
		}

		val plain = PairingCipher.decryptOrNull(key, String(body))
		if (plain == null) {
			// A stale QR's key, or noise. 400 rather than 403: the token
			// matched, so this is a confused sender, not an intruder.
			Log.w(TAG, "pairing: payload from $from did not decrypt")
			output.respondEmpty(400, "Bad Request")
			return false
		}
		val payload = runCatching {
			json.decodeFromString(PairPayload.serializer(), plain)
		}.getOrElse {
			Log.w(TAG, "pairing: payload from $from did not parse - ${it.message}")
			output.respondEmpty(400, "Bad Request")
			return false
		}

		Log.i(TAG, "pairing: accepted ${payload.servers.size} servers from $from")
		onReceived(payload.servers)
		output.respondEmpty(204, "No Content")
		return true
	}

	private fun BufferedOutputStream.respondEmpty(code: Int, reason: String) {
		write("HTTP/1.1 $code $reason\r\n".toByteArray())
		write("Content-Length: 0\r\nConnection: close\r\n\r\n".toByteArray())
		flush()
	}

	private fun newToken(): String {
		val buffer = ByteArray(16)
		random.nextBytes(buffer)
		return buffer.joinToString("") { "%02x".format(it) }
	}

	private companion object {
		const val TAG = "GainDrivePair"
		const val BACKLOG = 2
		/** Generous for a list of server logins; a cap, not a budget. */
		const val MAX_BODY_BYTES = 64 * 1024
		val random = SecureRandom()
	}
}
