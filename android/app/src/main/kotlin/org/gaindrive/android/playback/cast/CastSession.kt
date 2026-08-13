package org.gaindrive.android.playback.cast

import android.util.Log
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.Job
import kotlinx.coroutines.currentCoroutineContext
import kotlinx.coroutines.delay
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.asStateFlow
import kotlinx.coroutines.flow.filterNotNull
import kotlinx.coroutines.flow.first
import kotlinx.coroutines.isActive
import kotlinx.coroutines.launch
import kotlinx.coroutines.withTimeoutOrNull
import kotlinx.serialization.json.Json
import kotlinx.serialization.json.JsonObject
import kotlinx.serialization.json.buildJsonObject
import kotlinx.serialization.json.put
import java.util.concurrent.atomic.AtomicInteger
import javax.inject.Inject
import javax.inject.Singleton

/** What to play, and where the receiver should fetch it from. */
data class CastMedia(
	val url: String,
	/**
	 * Omitted from the LOAD when null, leaving the receiver to sniff. Prefer
	 * supplying it: the receiver picks its decode pipeline from this, and a
	 * wrong guess surfaces as a decode error minutes later.
	 */
	val mimeType: String?,
	/** Omitted from the LOAD when unknown; a stated zero would be a lie. */
	val durationSeconds: Double?,
	/** Where to start. The receiver seeks using the file's own index. */
	val startSeconds: Float = 0f,
	val title: String? = null,
	val artist: String? = null,
	val album: String? = null,
	val artworkUrl: String? = null,
	/** Selects the metadata block the receiver is given; see `metadata()`. */
	val isVideo: Boolean = false,
)

/**
 * The Cast v2 control channel: one live connection to a Chromecast, the
 * receiver's status as a flow, and the commands that drive it.
 *
 * Ported from `CastManager` in `src/castmanager.cc`. Two things are deliberately
 * different, and neither changes the protocol on the wire:
 *
 * * One multiplexed connection instead of a fresh one per command, per
 *   [CastChannel].
 * * Coroutines instead of a detached thread and a condition variable. The load
 *   generation survives as an [AtomicInteger] rather than becoming structured
 *   cancellation, because it means something slightly different: it invalidates
 *   work that is *already in flight on the receiver*, not just locally.
 *
 * What must not drift from the C++ is the retry behaviour in [LoadRetryWatcher]
 * and the diagnostics: the full `MEDIA_STATUS` and `RECEIVER_STATUS` payloads
 * are logged for every push, because receiver-side regressions cannot be
 * diagnosed from summarised state.
 */
@Singleton
class CastSession @Inject constructor(
	private val json: Json,
	private val wifi: WifiNetworks,
	private val scope: CoroutineScope,
) {

	private val _device = MutableStateFlow<CastDevice?>(null)
	val device: StateFlow<CastDevice?> = _device.asStateFlow()

	private val _status = MutableStateFlow(CastStatus())
	val status: StateFlow<CastStatus> = _status.asStateFlow()

	/** Non-null only while the receiver has our media session open. */
	private val transportId = MutableStateFlow<String?>(null)
	private val channel = MutableStateFlow<CastChannel?>(null)

	private var loop: Job? = null

	/**
	 * Bumped by every user-initiated LOAD. A worker checks it before each
	 * blocking step and abandons its LOAD if the user has since asked for
	 * something else. Retries deliberately do not bump it — a retry is the same
	 * intent as the load it repeats.
	 */
	private val loadGen = AtomicInteger(0)
	private val requestIds = AtomicInteger(1)

	private val retry = LoadRetryWatcher()

	/** The parameters of the last LOAD, so a retry can repeat it verbatim. */
	@Volatile
	private var lastLoad: CastMedia? = null

	// ── Session lifecycle ───────────────────────────────────────────────────

	fun connect(target: CastDevice) {
		if (_device.value == target && loop?.isActive == true) return
		// Switching devices tears down at once rather than going through
		// [disconnect]: the courtesy STOP there is asynchronous, and waiting for
		// it would delay the connection the user actually asked for.
		teardown()
		_device.value = target
		Log.i(TAG, "connecting to ${target.name} (${target.address}:${target.port})")
		loop = scope.launch(Dispatchers.IO) { runLoop(target) }
	}

	/**
	 * Leaves the receiver, stopping playback first so the TV does not sit on our
	 * abandoned session.
	 *
	 * The STOP has to go out *before* the loop is cancelled, because cancelling
	 * closes the socket it would travel on. Everything therefore happens in one
	 * coroutine, and it gives up its claim on the shared state if a [connect] has
	 * replaced the channel in the meantime.
	 */
	fun disconnect() {
		val open = channel.value
		val transport = transportId.value
		val msid = _status.value.mediaSessionId
		val name = _device.value?.name
		if (open == null || transport == null) {
			teardown()
			return
		}
		scope.launch(Dispatchers.IO) {
			open.send(CastNs.MEDIA, transport, buildJsonObject {
				put("type", "STOP")
				put("requestId", requestIds.getAndIncrement())
				put("mediaSessionId", msid)
			})
			// Long enough for the receiver to act on it; the socket dies next.
			delay(STOP_GRACE_MS)
			if (channel.value === open) {
				teardown()
				Log.i(TAG, "disconnected from $name")
			}
		}
	}

	/** Synchronous half of leaving: cancel, close, forget. */
	private fun teardown() {
		loop?.cancel()
		loop = null
		channel.value?.close()
		channel.value = null
		_device.value = null
		transportId.value = null
		_status.value = CastStatus()
		retry.disarm()
		lastLoad = null
	}

	// ── Commands ────────────────────────────────────────────────────────────

	fun load(media: CastMedia) {
		val gen = loadGen.incrementAndGet()
		Log.i(TAG, "load gen=$gen url=${media.url} start=${media.startSeconds}")

		// Reset the visible status for the new track, seeding the duration we
		// were told so the UI has one before the receiver reports its own, and
		// arm the retry against the session this LOAD is about to replace.
		retry.arm(_status.value.mediaSessionId)
		_status.value = CastStatus(duration = media.durationSeconds?.toFloat() ?: 0f)
		lastLoad = media

		scope.launch(Dispatchers.IO) { sendLoad(media, gen) }
	}

	fun play() = mediaCommand("PLAY")

	fun pause() = mediaCommand("PAUSE")

	fun seek(seconds: Float) = mediaCommand("SEEK") { put("currentTime", seconds) }

	fun stopPlayback() = mediaCommand("STOP")

	private fun mediaCommand(type: String, extra: JsonObjectBuilderScope? = null) {
		scope.launch(Dispatchers.IO) {
			val open = awaitChannel() ?: return@launch
			val transport = transportId.value ?: return@launch
			val payload = buildJsonObject {
				put("type", type)
				put("requestId", requestIds.getAndIncrement())
				put("mediaSessionId", _status.value.mediaSessionId)
				extra?.invoke(this)
			}
			Log.i(TAG, "$type sent")
			open.send(CastNs.MEDIA, transport, payload)
		}
	}

	// ── The connection ──────────────────────────────────────────────────────

	private suspend fun runLoop(target: CastDevice) {
		while (currentCoroutineContext().isActive) {
			val open = CastChannel.open(target, json, wifi)
			if (open == null) {
				Log.w(TAG, "connect failed, retrying")
				delay(RECONNECT_DELAY_MS)
				continue
			}
			channel.value = open
			try {
				// The virtual connection to the platform must exist before
				// anything else is accepted; the app's own transport gets its
				// own CONNECT once a RECEIVER_STATUS names it.
				open.send(CastNs.CONNECTION, CastNs.RECEIVER_ID, connectPayload())
				open.send(CastNs.RECEIVER, CastNs.RECEIVER_ID, request("GET_STATUS"))
				pump(open)
				// Reached only when the receiver closed on us. Logged because an
				// idle connection and one that is silently reconnecting every few
				// seconds otherwise look identical from outside — and the app
				// sends nothing at all between sessions, which is when a receiver
				// is most likely to hang up.
				Log.i(TAG, "connection closed by receiver, reconnecting")
			} finally {
				open.close()
				channel.value = null
				transportId.value = null
			}
			if (currentCoroutineContext().isActive) delay(RECONNECT_DELAY_MS)
		}
	}

	private suspend fun pump(open: CastChannel) {
		while (currentCoroutineContext().isActive) {
			when (val rx = open.receive()) {
				CastRx.Closed -> return
				// The receiver pushes only on state changes, so steady playback
				// would report no position at all without an explicit poll.
				CastRx.Idle -> transportId.value?.let {
					open.send(CastNs.MEDIA, it, request("GET_STATUS"))
				}

				is CastRx.Message -> handle(open, rx.json)
			}
		}
	}

	private suspend fun handle(open: CastChannel, message: JsonObject) {
		when (CastStatus.typeOf(message)) {
			"PING" -> open.send(CastNs.HEARTBEAT, CastNs.RECEIVER_ID, buildJsonObject {
				put("type", "PONG")
			})

			"RECEIVER_STATUS" -> {
				// Logged whole: STOP and idle teardown both invalidate our
				// transport, and this is the only warning of either.
				Log.i(TAG, "rx RECEIVER_STATUS: $message")
				onReceiverStatus(open, message)
			}

			"MEDIA_STATUS" -> {
				Log.i(TAG, "rx MEDIA_STATUS: $message")
				CastStatus.parse(message)?.let(::onMediaStatus)
			}

			"ERROR" -> Log.w(TAG, "rx ERROR: $message")
			"PONG" -> Unit
			else -> Log.i(TAG, "rx ${CastStatus.typeOf(message)}")
		}
	}

	private suspend fun onReceiverStatus(open: CastChannel, message: JsonObject) {
		val transport = CastStatus.transportIdOf(message)
		if (transport == null) {
			transportId.value = null
			return
		}
		if (transport == transportId.value) return

		// Connect to the app's transport before publishing it: everything that
		// waits on transportId goes straight on to send a media command, and the
		// receiver drops those until the virtual connection exists.
		open.send(CastNs.CONNECTION, transport, connectPayload())
		transportId.value = transport
		open.send(CastNs.MEDIA, transport, request("GET_STATUS"))
	}

	private fun onMediaStatus(fresh: CastStatus) {
		// A push mid-playback carries no `media` block, so duration arrives once
		// and must be carried forward rather than collapsing to zero.
		val merged =
			if (fresh.duration == 0f) fresh.copy(duration = _status.value.duration) else fresh
		_status.value = merged

		if (retry.onStatus(merged)) {
			val media = lastLoad ?: return
			val gen = loadGen.get()
			Log.w(TAG, "auto-retry LOAD (gen=$gen) — receiver went IDLE/ERROR")
			scope.launch(Dispatchers.IO) { sendLoad(media, gen) }
		}
	}

	private suspend fun sendLoad(media: CastMedia, gen: Int) {
		if (loadGen.get() != gen) return
		val open = awaitChannel() ?: return
		if (loadGen.get() != gen) return

		val transport = ensureTransport(open) ?: run {
			Log.w(TAG, "no transportId from receiver; LOAD abandoned")
			return
		}
		if (loadGen.get() != gen) return

		val payload = buildJsonObject {
			put("type", "LOAD")
			put("requestId", requestIds.getAndIncrement())
			// Explicit even though the spec defaults it true — some receiver
			// versions have been quirky about it, and it costs nothing.
			put("autoplay", true)
			if (media.startSeconds > 0f) put("currentTime", media.startSeconds)
			put("media", buildJsonObject {
				put("contentId", media.url)
				media.mimeType?.let { put("contentType", it) }
				put("streamType", "BUFFERED")
				// Redundant against the receiver's own parsing, but it gives an
				// early hint before any byte-range request is made.
				media.durationSeconds?.let { put("duration", it) }
				media.metadata()?.let { put("metadata", it) }
			})
		}
		Log.i(TAG, "LOAD $payload")
		open.send(CastNs.MEDIA, transport, payload)
	}

	/**
	 * The transport of a running Default Media Receiver, launching one if there
	 * is none.
	 *
	 * Asking before launching is not just politeness: LAUNCH against an already
	 * running app tears it down and recreates it, which loses the session we may
	 * be about to load into.
	 */
	private suspend fun ensureTransport(open: CastChannel): String? {
		transportId.value?.let { return it }

		open.send(CastNs.RECEIVER, CastNs.RECEIVER_ID, request("GET_STATUS"))
		awaitTransport(STATUS_WAIT_MS)?.let { return it }

		open.send(CastNs.RECEIVER, CastNs.RECEIVER_ID, buildJsonObject {
			put("type", "LAUNCH")
			put("appId", CastNs.DEFAULT_MEDIA_APP)
			put("requestId", requestIds.getAndIncrement())
		})
		return awaitTransport(LAUNCH_WAIT_MS)
	}

	private suspend fun awaitChannel(): CastChannel? =
		withTimeoutOrNull(CHANNEL_WAIT_MS) { channel.filterNotNull().first() }

	private suspend fun awaitTransport(timeoutMs: Long): String? =
		withTimeoutOrNull(timeoutMs) { transportId.filterNotNull().first() }

	private fun request(type: String): JsonObject = buildJsonObject {
		put("type", type)
		put("requestId", requestIds.getAndIncrement())
	}

	private fun connectPayload(): JsonObject = buildJsonObject {
		put("type", "CONNECT")
	}

	/**
	 * The receiver shows this on the TV while playing. The C++ sends none
	 * because the web client is the only thing looking at the browser's own UI;
	 * on a phone the television is the second screen and the one people watch.
	 *
	 * A branch rather than a changed constant: `artist` and `albumName` are
	 * fields of a music track and not of a movie, so sending them under
	 * `metadataType: 1` would put nothing on screen. The album line carries a
	 * film's section — `Documentaries`, a series name — which is what `subtitle`
	 * is for.
	 */
	private fun CastMedia.metadata(): JsonObject? {
		if (title == null && artist == null && album == null && artworkUrl == null) return null
		return buildJsonObject {
			if (isVideo) {
				// 1 = MovieMediaMetadata.
				put("metadataType", 1)
				title?.let { put("title", it) }
				album?.let { put("subtitle", it) }
			} else {
				// 3 = MusicTrackMediaMetadata.
				put("metadataType", 3)
				title?.let { put("title", it) }
				artist?.let { put("artist", it) }
				album?.let { put("albumName", it) }
			}
			artworkUrl?.let {
				put("images", kotlinx.serialization.json.buildJsonArray {
					add(buildJsonObject { put("url", it) })
				})
			}
		}
	}

	private companion object {
		const val TAG = "GainDriveCast"
		const val RECONNECT_DELAY_MS = 500L
		const val CHANNEL_WAIT_MS = 5_000L
		const val STATUS_WAIT_MS = 1_500L
		const val LAUNCH_WAIT_MS = 10_000L
		const val STOP_GRACE_MS = 300L
	}
}

/** Lets [CastSession.mediaCommand] take extra fields without a builder class. */
private typealias JsonObjectBuilderScope =
	kotlinx.serialization.json.JsonObjectBuilder.() -> Unit
