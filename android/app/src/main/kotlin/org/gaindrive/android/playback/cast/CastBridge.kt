package org.gaindrive.android.playback.cast

import android.content.Context
import android.net.Uri
import android.net.wifi.WifiManager
import android.util.Log
import androidx.media3.common.C
import androidx.media3.datasource.DataSpec
import dagger.hilt.android.qualifiers.ApplicationContext
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.Job
import kotlinx.coroutines.launch
import okhttp3.OkHttpClient
import okhttp3.Request
import org.gaindrive.android.data.cache.AudioCache
import java.io.BufferedOutputStream
import java.io.IOException
import java.net.ServerSocket
import java.net.Socket
import java.security.SecureRandom
import java.util.Collections
import java.util.concurrent.TimeUnit
import javax.inject.Inject
import javax.inject.Singleton

/**
 * Relays audio from a GainDrive server to a Cast receiver that cannot reach that
 * server itself.
 *
 * Cast is pull-only - the sender hands over a URL and the receiver performs its
 * own HTTP GET - so when the phone is the only thing that can reach the server,
 * the phone has to carry the bytes. This is the technique BubbleUPnP, LocalCast
 * and VLC all use; the Cast protocol neither provides it nor prevents it, since
 * the receiver only ever sees an ordinary HTTP endpoint.
 *
 * Deliberately hand-rolled on [ServerSocket]. It serves one known client and
 * parses little more than the request line and `Range`, so a library would buy
 * little, and the socket has to be bound to a specific interface.
 *
 * It serves two kinds of thing, and the receiver cannot tell them apart: bytes
 * fetched from the server on the phone's behalf, and bytes already on the device
 * because the track was downloaded. The second is not merely an optimisation -
 * it is the only one that works with no connectivity at all, which is the state
 * a downloaded library exists for.
 *
 * **Security.** The bridge must never become an open proxy into a private music
 * library, so: an unguessable token, rotated per session, is required in the
 * path; requests from outside the local Wi-Fi subnet are refused; and only
 * resources that have been [publish]ed or [publishLocal]ed for the current queue
 * can be fetched at all - this is not a general gateway keyed on song id.
 */
@Singleton
class CastBridge @Inject constructor(
	@ApplicationContext private val context: Context,
	private val wifi: WifiNetworks,
	private val httpClient: OkHttpClient,
	private val audioCache: AudioCache,
	private val scope: CoroutineScope,
) {

	/**
	 * The shared client with a read timeout long enough for the server to build
	 * a video remux, whose transcode cache does not answer until ffmpeg has
	 * finished. [CastUrls] warms that before the receiver ever asks, so this is
	 * the backstop for a warm that did not happen or did not work; 30 s - the
	 * shared client's figure, sized for audio - would turn it into a failure.
	 */
	private val upstreamClient by lazy {
		httpClient.newBuilder().readTimeout(UPSTREAM_TIMEOUT_MINUTES, TimeUnit.MINUTES).build()
	}

	private var server: ServerSocket? = null
	private var acceptJob: Job? = null
	private var wifiLock: WifiManager.WifiLock? = null
	private var token: String = ""
	private var origin: String = ""

	/** Where a published resource's bytes come from. */
	private sealed interface Resource {
		/** Fetched from the owning server, on the phone's default route. */
		class Upstream(val url: String) : Resource

		/**
		 * Read out of the byte cache, for a track already downloaded.
		 *
		 * The length is carried rather than looked up because it is what decided
		 * this resource could be published at all: the cache does not always know
		 * one, and without it there is no `Content-Length` and no way to answer a
		 * ranged request - which is most of what a receiver asks for.
		 */
		class Local(val cacheKey: String, val mimeType: String?, val length: Long) : Resource
	}

	/**
	 * Published resources by opaque key. Bounded because a long queue would
	 * otherwise accumulate one entry per track played; a handful covers the track
	 * playing, its artwork, and the seeks and re-loads around them.
	 */
	private val published = Collections.synchronizedMap(
		object : LinkedHashMap<String, Resource>(16, 0.75f, true) {
			override fun removeEldestEntry(eldest: Map.Entry<String, Resource>): Boolean =
				size > MAX_PUBLISHED && eldest.key !in pinned
		}
	)

	/**
	 * Keys belonging to the load currently on the receiver, exempt from
	 * eviction until the next one replaces them.
	 *
	 * **Subtitle tracks are why this exists.** The map is access-ordered, and a
	 * caption is published at LOAD but not fetched until somebody turns it on -
	 * so with only use to go by it is the least recently used thing there is,
	 * and every seek publishes a fresh stream key that ages it further. A
	 * viewer switching subtitles forty minutes into a film would find the key
	 * evicted, get a 404 the receiver reports nowhere, and see nothing happen.
	 * Age is the wrong measure for a resource whose whole purpose is to be
	 * available later.
	 */
	private val pinned = Collections.synchronizedSet(mutableSetOf<String>())

	/**
	 * Declares that everything published from now until the next call belongs
	 * to one load. The previous load's keys become evictable again.
	 */
	@Synchronized
	fun beginLoad() {
		pinned.clear()
	}

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
			// answer is to exclude the local subnet from AllowedIPs.
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
	fun publish(upstreamUrl: String): String? = publish(Resource.Upstream(upstreamUrl))

	/**
	 * Makes bytes already held under [cacheKey] fetchable through the bridge.
	 *
	 * [length] is the caller's assertion that the whole track is stored and how
	 * long it is; the bridge does not re-check, because the caller had to know
	 * both to decide to come here rather than publish a server URL.
	 */
	fun publishLocal(cacheKey: String, mimeType: String?, length: Long): String? =
		publish(Resource.Local(cacheKey, mimeType, length))

	@Synchronized
	private fun publish(resource: Resource): String? {
		if (server == null && !start()) return null
		val key = newKey()
		published[key] = resource
		pinned.add(key)
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
		val from = client.inetAddress?.hostAddress ?: "?"
		client.use {
			runCatching { handle(client) }.onFailure {
				// A receiver closing mid-track is ordinary rather than an error -
				// it does that on every seek - but an upstream failure arrives as
				// an IOException too, so both are reported and the message tells
				// them apart.
				val level = if (it is IOException) Log.INFO else Log.WARN
				Log.println(level, TAG, "bridge $from: ended - ${it.message}")
			}
		}
	}

	private fun handle(client: Socket) {
		val output = BufferedOutputStream(client.getOutputStream())
		val from = client.inetAddress?.hostAddress ?: "?"

		if (!wifi.isOnLocalSubnet(client.inetAddress)) {
			Log.w(TAG, "bridge refused $from: not on the local subnet")
			output.respondEmpty(403, "Forbidden")
			return
		}

		val request = when (val read = readRequest(client)) {
			is BridgeRead.Ok -> read
			// Not an error, and not answered: there is nothing to answer, and
			// the peer has already gone.
			BridgeRead.Silent -> {
				Log.i(TAG, "bridge $from: opened and closed without a request")
				return
			}
			is BridgeRead.Malformed -> {
				Log.w(TAG, "bridge $from: ${read.why}")
				output.respondEmpty(400, "Bad Request")
				return
			}
		}

		// Answered before the key is resolved: a preflight asks whether the
		// request would be allowed, not for the resource, and a receiver that
		// preflighted an evicted key would learn nothing useful from a 404.
		if (request.method == "OPTIONS") {
			output.respondEmpty(204, "No Content")
			return
		}

		val resource = resolve(request.path) ?: run {
			// Logged without the path, which carries the session token.
			Log.w(TAG, "bridge $from: ${request.method} for an unpublished key")
			output.respondEmpty(404, "Not Found")
			return
		}

		// One line per request, and the reason this exists: whether a receiver
		// ever reaches the bridge is the difference between a routing problem
		// and a relaying problem, and nothing else in the system can tell them
		// apart. The token is deliberately not logged.
		Log.i(TAG, "bridge $from: ${request.method}${request.range?.let { " $it" }.orEmpty()}")

		when (resource) {
			is Resource.Local -> serveStored(output, from, resource, request)
			is Resource.Upstream -> serveUpstream(output, from, resource.url, request)
		}
	}

	/**
	 * Serves bytes the device already holds, without touching the network.
	 *
	 * The response is built here rather than relayed, so `Range` has to be
	 * honoured rather than passed on: receivers read the tail of an Ogg or FLAC
	 * for its duration and seek table before playing a note, and a server that
	 * answers 200 to all of them hands over the whole track each time.
	 */
	private fun serveStored(
		output: BufferedOutputStream,
		from: String,
		resource: Resource.Local,
		request: BridgeRead.Ok,
	) {
		val range = parseRange(request.range, resource.length)
		val start = range?.first ?: 0
		val end = range?.last ?: (resource.length - 1)
		val count = end - start + 1

		Log.i(TAG, "bridge $from: stored $start-$end/${resource.length}")

		// Opened before a byte of the response is written. Committing to a status
		// line and a Content-Length and only then discovering the bytes are not
		// there would leave the receiver with a truncated body and no error -
		// which it reports much later, as its own network timeout.
		val source = audioCache.readOnlySource()
		try {
			source.open(
				DataSpec.Builder()
					// The key, not the URI, is what selects the stored bytes;
					// CacheDataSource only falls back to the URI when no key is
					// given, and there is no URI here to give.
					.setUri(Uri.EMPTY)
					.setKey(resource.cacheKey)
					.setPosition(start)
					.setLength(count)
					.build()
			)

			output.write(
				if (range != null) "HTTP/1.1 206 Partial Content\r\n".toByteArray()
				else "HTTP/1.1 200 OK\r\n".toByteArray()
			)
			output.writeCors()
			resource.mimeType?.let { output.write("Content-Type: $it\r\n".toByteArray()) }
			output.write("Content-Length: $count\r\n".toByteArray())
			output.write("Accept-Ranges: bytes\r\n".toByteArray())
			if (range != null) {
				output.write("Content-Range: bytes $start-$end/${resource.length}\r\n".toByteArray())
			}
			output.write("Connection: close\r\n\r\n".toByteArray())

			if (request.method != "HEAD") {
				val buffer = ByteArray(COPY_BUFFER)
				while (true) {
					val n = source.read(buffer, 0, buffer.size)
					if (n == C.RESULT_END_OF_INPUT) break
					output.write(buffer, 0, n)
				}
			}
			output.flush()
		} finally {
			runCatching { source.close() }
		}
	}

	/**
	 * `bytes=start-end`, `bytes=start-` or `bytes=-count`, and null for anything
	 * else - including a range that runs off the end, which is answered whole
	 * rather than with a 416 no receiver is going to act on.
	 */
	private fun parseRange(header: String?, length: Long): LongRange? {
		val spec = header?.trim() ?: return null
		if (!spec.startsWith("bytes", ignoreCase = true)) return null
		val parts = spec.substringAfter('=').substringBefore(',').split('-')
		if (parts.size != 2) return null

		val startText = parts[0].trim()
		val endText = parts[1].trim()
		if (startText.isEmpty()) {
			val count = endText.toLongOrNull()?.takeIf { it > 0 } ?: return null
			return (length - minOf(count, length))..(length - 1)
		}
		val start = startText.toLongOrNull() ?: return null
		val end = if (endText.isEmpty()) length - 1 else (endText.toLongOrNull() ?: return null)
		if (start < 0 || start > end || end >= length) return null
		return start..end
	}

	private fun serveUpstream(
		output: BufferedOutputStream,
		from: String,
		upstream: String,
		request: BridgeRead.Ok,
	) {
		val builder = Request.Builder().url(upstream)
		// Passed through unchanged, and not optional: receivers issue ranged
		// requests to read FLAC and Ogg seek tables, and seeking depends on the
		// server answering them.
		request.range?.let { builder.header("Range", it) }
		if (request.method == "HEAD") builder.head()

		upstreamClient.newCall(builder.build()).execute().use { response ->
			Log.i(
				TAG,
				"bridge $from: upstream ${response.code}" +
					" len=${response.header("Content-Length") ?: "?"}" +
					(response.header("Content-Range")?.let { " range=$it" } ?: ""),
			)
			output.write("HTTP/1.1 ${response.code} ${response.message}\r\n".toByteArray())
			// Ours, not relayed: the upstream server sends its own, and
			// forwarding both would put two Access-Control-Allow-Origin
			// headers on one response, which a browser treats as neither.
			output.writeCors()
			for (header in RELAYED_HEADERS) {
				response.header(header)?.let {
					output.write("$header: $it\r\n".toByteArray())
				}
			}
			output.write("Connection: close\r\n\r\n".toByteArray())
			if (request.method != "HEAD") {
				response.body.byteStream().copyTo(output, COPY_BUFFER)
			}
			output.flush()
		}
	}

	/** `/<token>/<key>`, and nothing else is servable. */
	private fun resolve(path: String): Resource? {
		if (token.isEmpty()) return null
		val parts = path.trim('/').split('/')
		if (parts.size != 2) return null
		if (parts[0] != token) return null
		return published[parts[1]]
	}

	/** What [readRequest] found. */
	private sealed interface BridgeRead {
		class Ok(val method: String, val path: String, val range: String?) : BridgeRead

		/**
		 * The peer opened a connection and closed it without sending anything.
		 *
		 * Receivers do this routinely - the Default Media Receiver opens a
		 * speculative socket alongside the one it fetches on - so it is not a
		 * failure and must not read as one. On the first successful cast it was
		 * reported as `unparseable request`, a warning and a 400 for something
		 * that was never a request in the first place, sitting in a log where
		 * every other line was being read as evidence.
		 */
		data object Silent : BridgeRead

		/** Never carries the request line: the path holds the session token. */
		class Malformed(val why: String) : BridgeRead
	}

	private fun readRequest(client: Socket): BridgeRead {
		val reader = client.getInputStream().bufferedReader()
		val requestLine = reader.readLine() ?: return BridgeRead.Silent
		val parts = requestLine.split(' ')
		if (parts.size < 2) return BridgeRead.Malformed("malformed request line")
		val method = parts[0].uppercase()
		// OPTIONS is answered, not served: the receiver fetches a side-loaded
		// subtitle track by XHR, and a cross-origin XHR may preflight. Treating
		// it as malformed would fail the caption and, with it, the LOAD.
		if (method != "GET" && method != "HEAD" && method != "OPTIONS") {
			return BridgeRead.Malformed("unsupported method $method")
		}

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
		return BridgeRead.Ok(method, parts[1], range)
	}

	/**
	 * The headers that let the receiver *read* what it fetched.
	 *
	 * A side-loaded subtitle track is fetched by XHR from the receiver app's
	 * own origin, so without these the browser inside the Chromecast discards a
	 * response it has already downloaded - and because declaring any track puts
	 * the media element into anonymous cross-origin mode, the film needs them
	 * too. On the direct route the gaindrive server sends its own; here the
	 * bridge is the origin, so it has to.
	 */
	private fun BufferedOutputStream.writeCors() {
		write("Access-Control-Allow-Origin: *\r\n".toByteArray())
		write("Access-Control-Allow-Methods: GET, HEAD, OPTIONS\r\n".toByteArray())
		write("Access-Control-Allow-Headers: Range, Content-Type\r\n".toByteArray())
		write(
			("Access-Control-Expose-Headers: " +
				"Content-Length, Content-Range, Accept-Ranges\r\n").toByteArray()
		)
	}

	private fun BufferedOutputStream.respondEmpty(code: Int, reason: String) {
		write("HTTP/1.1 $code $reason\r\n".toByteArray())
		writeCors()
		write("Content-Length: 0\r\nConnection: close\r\n\r\n".toByteArray())
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
		// HIGH_PERF although deprecated: the suggested replacement,
		// WIFI_MODE_FULL_LOW_LATENCY, deactivates when the screen goes off,
		// which is the one moment this lock exists for. On API 34+ HIGH_PERF
		// degrades to WIFI_MODE_FULL by itself; older devices still honour it.
		@Suppress("DEPRECATION")
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
		// One load is a stream, its artwork and one entry per subtitle track,
		// and the entries of the load before last are still worth keeping for
		// a receiver that re-requests. Raised from 16 when captions arrived:
		// they multiply the per-load count, and an entry is a URL string.
		const val MAX_PUBLISHED = 64
		const val COPY_BUFFER = 64 * 1024

		/** Long enough for a whole-film remux to finish; see [upstreamClient]. */
		const val UPSTREAM_TIMEOUT_MINUTES = 10L

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
