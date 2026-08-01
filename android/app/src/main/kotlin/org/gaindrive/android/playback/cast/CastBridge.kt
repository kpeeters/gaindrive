package org.gaindrive.android.playback.cast

import android.content.Context
import android.net.wifi.WifiManager
import android.util.Log
import dagger.hilt.android.qualifiers.ApplicationContext
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.Job
import kotlinx.coroutines.launch
import okhttp3.OkHttpClient
import okhttp3.Request
import java.io.BufferedOutputStream
import java.io.IOException
import java.net.ServerSocket
import java.net.Socket
import java.security.SecureRandom
import java.util.Collections
import javax.inject.Inject
import javax.inject.Singleton

/**
 * Relays audio from a GainDrive server to a Cast receiver that cannot reach that
 * server itself.
 *
 * Cast is pull-only — the sender hands over a URL and the receiver performs its
 * own HTTP GET — so when the phone is the only thing that can reach the server,
 * the phone has to carry the bytes. This is the technique BubbleUPnP, LocalCast
 * and VLC all use; the Cast protocol neither provides it nor prevents it, since
 * the receiver only ever sees an ordinary HTTP endpoint.
 *
 * Deliberately hand-rolled on [ServerSocket]. It serves one known client and
 * parses little more than the request line and `Range`, so a library would buy
 * little, and the socket has to be bound to a specific interface.
 *
 * **Security.** The bridge must never become an open proxy into a private music
 * library, so: an unguessable token, rotated per session, is required in the
 * path; requests from outside the local Wi-Fi subnet are refused; and only URLs
 * that have been [publish]ed for the current queue can be fetched at all — this
 * is not a general gateway keyed on song id.
 */
@Singleton
class CastBridge @Inject constructor(
	@ApplicationContext private val context: Context,
	private val wifi: WifiNetworks,
	private val httpClient: OkHttpClient,
	private val scope: CoroutineScope,
) {

	private var server: ServerSocket? = null
	private var acceptJob: Job? = null
	private var wifiLock: WifiManager.WifiLock? = null
	private var token: String = ""
	private var origin: String = ""

	/**
	 * Published upstream URLs by opaque key. Bounded because a long queue would
	 * otherwise accumulate one entry per track played; a handful covers the track
	 * playing, its artwork, and the seeks and re-loads around them.
	 */
	private val published = Collections.synchronizedMap(
		object : LinkedHashMap<String, String>(16, 0.75f, true) {
			override fun removeEldestEntry(eldest: Map.Entry<String, String>): Boolean =
				size > MAX_PUBLISHED
		}
	)

	@Synchronized
	fun start(): Boolean {
		if (server != null) return true

		val address = wifi.localAddress()
		if (address == null) {
			Log.w(TAG, "no Wi-Fi address; cannot bridge")
			return false
		}

		return runCatching {
			// Bound to the Wi-Fi address rather than to the Wi-Fi Network: there
			// is no Network.bindSocket for a ServerSocket. Under a full-tunnel
			// VPN the reply route is the remaining unknown, and the documented
			// answer is to exclude the local subnet from AllowedIPs — see
			// CAST.md, "The routing problem".
			val socket = ServerSocket(0, BACKLOG, address)
			server = socket
			token = newToken()
			origin = "http://${address.hostAddress}:${socket.localPort}"
			acquireWifiLock()
			acceptJob = scope.launch(Dispatchers.IO) { acceptLoop(socket) }
			Log.i(TAG, "bridge listening on $origin")
			true
		}.getOrElse {
			Log.w(TAG, "bridge failed to start: ${it.message}")
			server = null
			false
		}
	}

	@Synchronized
	fun stop() {
		acceptJob?.cancel()
		acceptJob = null
		runCatching { server?.close() }
		server = null
		published.clear()
		token = ""
		origin = ""
		releaseWifiLock()
	}

	/**
	 * Makes [upstreamUrl] fetchable through the bridge and returns the URL to
	 * hand the receiver, or null if the bridge is not running.
	 *
	 * Used for audio and cover art alike: artwork lives on the same unreachable
	 * server, and a receiver that cannot fetch the audio cannot fetch the sleeve
	 * either.
	 */
	@Synchronized
	fun publish(upstreamUrl: String): String? {
		if (server == null && !start()) return null
		val key = newKey()
		published[key] = upstreamUrl
		return "$origin/$token/$key"
	}

	private suspend fun acceptLoop(socket: ServerSocket) {
		while (true) {
			val client = try {
				socket.accept()
			} catch (_: IOException) {
				return // closed, or the network went away
			}
			// Its own coroutine: receivers routinely probe metadata on one
			// connection while playing on another, and a serial loop would
			// deadlock exactly there.
			scope.launch(Dispatchers.IO) { serve(client) }
		}
	}

	private fun serve(client: Socket) {
		client.use {
			runCatching { handle(client) }.onFailure {
				// A receiver closing mid-track is ordinary, not an error.
				if (it !is IOException) Log.w(TAG, "bridge request failed: ${it.message}")
			}
		}
	}

	private fun handle(client: Socket) {
		val output = BufferedOutputStream(client.getOutputStream())

		if (!wifi.isOnLocalSubnet(client.inetAddress)) {
			Log.w(TAG, "refused ${client.inetAddress}: not on the local subnet")
			output.respondEmpty(403, "Forbidden")
			return
		}

		val request = readRequest(client) ?: run {
			output.respondEmpty(400, "Bad Request")
			return
		}

		val upstream = resolve(request.path) ?: run {
			output.respondEmpty(404, "Not Found")
			return
		}

		val builder = Request.Builder().url(upstream)
		// Passed through unchanged, and not optional: receivers issue ranged
		// requests to read FLAC and Ogg seek tables, and seeking depends on the
		// server answering them.
		request.range?.let { builder.header("Range", it) }
		if (request.method == "HEAD") builder.head()

		httpClient.newCall(builder.build()).execute().use { response ->
			output.write("HTTP/1.1 ${response.code} ${response.message}\r\n".toByteArray())
			for (header in RELAYED_HEADERS) {
				response.header(header)?.let {
					output.write("$header: $it\r\n".toByteArray())
				}
			}
			output.write("Connection: close\r\n\r\n".toByteArray())
			if (request.method != "HEAD") {
				response.body?.byteStream()?.copyTo(output, COPY_BUFFER)
			}
			output.flush()
		}
	}

	/** `/<token>/<key>`, and nothing else is servable. */
	private fun resolve(path: String): String? {
		if (token.isEmpty()) return null
		val parts = path.trim('/').split('/')
		if (parts.size != 2) return null
		if (parts[0] != token) return null
		return published[parts[1]]
	}

	private class BridgeRequest(val method: String, val path: String, val range: String?)

	private fun readRequest(client: Socket): BridgeRequest? {
		val reader = client.getInputStream().bufferedReader()
		val requestLine = reader.readLine() ?: return null
		val parts = requestLine.split(' ')
		if (parts.size < 2) return null
		val method = parts[0].uppercase()
		if (method != "GET" && method != "HEAD") return null

		var range: String? = null
		while (true) {
			val line = reader.readLine() ?: break
			if (line.isEmpty()) break
			val split = line.indexOf(':')
			if (split <= 0) continue
			if (line.substring(0, split).equals("Range", ignoreCase = true)) {
				range = line.substring(split + 1).trim()
			}
		}
		return BridgeRequest(method, parts[1], range)
	}

	private fun BufferedOutputStream.respondEmpty(code: Int, reason: String) {
		write("HTTP/1.1 $code $reason\r\nContent-Length: 0\r\nConnection: close\r\n\r\n".toByteArray())
		flush()
	}

	/**
	 * Wi-Fi power saving otherwise stalls the relay once the screen goes off,
	 * which is exactly when a cast session is left running.
	 */
	private fun acquireWifiLock() {
		if (wifiLock != null) return
		val manager = context.applicationContext
			.getSystemService(Context.WIFI_SERVICE) as? WifiManager ?: return
		wifiLock = runCatching {
			manager.createWifiLock(WifiManager.WIFI_MODE_FULL_HIGH_PERF, "gaindrive:cast")
				.apply { acquire() }
		}.getOrNull()
	}

	private fun releaseWifiLock() {
		runCatching { wifiLock?.takeIf { it.isHeld }?.release() }
		wifiLock = null
	}

	private fun newToken(): String = randomHex(16)

	private fun newKey(): String = randomHex(8)

	private fun randomHex(bytes: Int): String {
		val buffer = ByteArray(bytes)
		random.nextBytes(buffer)
		return buffer.joinToString("") { "%02x".format(it) }
	}

	private companion object {
		const val TAG = "GainDriveCast"
		const val BACKLOG = 8
		const val MAX_PUBLISHED = 16
		const val COPY_BUFFER = 64 * 1024

		val random = SecureRandom()

		/**
		 * Everything the receiver needs to seek and to size the stream. Anything
		 * else is the upstream server's business, not the receiver's.
		 */
		val RELAYED_HEADERS = listOf(
			"Content-Type",
			"Content-Length",
			"Content-Range",
			"Accept-Ranges",
		)
	}
}
