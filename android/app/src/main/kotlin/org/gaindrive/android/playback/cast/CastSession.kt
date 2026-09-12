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
import kotlinx.serialization.json.add
import kotlinx.serialization.json.addJsonObject
import kotlinx.serialization.json.buildJsonObject
import kotlinx.serialization.json.put
import kotlinx.serialization.json.putJsonArray
import org.gaindrive.android.data.model.AudioQuality
import java.util.concurrent.atomic.AtomicInteger
import javax.inject.Inject
import javax.inject.Singleton

/**
 * One side-loaded subtitle track, as the receiver is told about it.
 *
 * [trackId] is 1-based and is what `EDIT_TRACKS_INFO` names; [url] is fetched
 * by the receiver itself, so on the relayed route it must be a bridge URL and
 * not the server's.
 */
data class CastCaption(
	val trackId: Int,
	val url: String,
	val label: String,
	/**
	 * Required by the receiver for a subtitle track, and a track without one
	 * can be dropped with no diagnostic anywhere — hence the ISO 639-2 code
	 * for "undetermined" rather than an empty string.
	 */
	val language: String = "und",
)

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
	/**
	 * Who is serving [url]. Carried here rather than left in [CastUrls] because
	 * this is the only record of what a live session is doing — the decision is
	 * made per track and there is nothing else holding it afterwards.
	 */
	val route: CastRoute = CastRoute.DIRECT,
	/** What the server was asked to send. Null for video, which is never asked. */
	val quality: AudioQuality? = null,
	/**
	 * **Declared in every LOAD, whether or not one is switched on.**
	 * `EDIT_TRACKS_INFO` can activate a trackId the LOAD declared but cannot
	 * introduce one, so a track omitted here is one the viewer can never reach
	 * without reloading the film — which shows up as subtitles that work when
	 * chosen before playback and never when chosen during it.
	 */
	val captions: List<CastCaption> = emptyList(),
	/** Which of [captions] are on, by trackId. Empty is a valid answer. */
	val activeTrackIds: List<Int> = emptyList(),
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

	private val _message = MutableStateFlow<String?>(null)

	/**
	 * Something that could not be cast, in words, for the shell to show.
	 *
	 * Here rather than on [CastPlayer] because that object is rebuilt per
	 * session while this one is the singleton every observer already holds —
	 * the same arrangement `PlaybackWatchdog.message` uses, and it is
	 * `PlayerConnection` that collects both.
	 *
	 * The case it exists for: a queue already holding a film that cannot be
	 * cast when a device is connected. The enqueue checks refuse such a film
	 * up front, but neither covers connecting to a queue that has one in it,
	 * and the alternative is [CastPlayer] skipping the track with nothing on
	 * screen to say why.
	 */
	val message: StateFlow<String?> = _message.asStateFlow()

	fun report(text: String) { _message.value = text }

	fun consumeMessage() { _message.value = null }

	/**
	 * How many `MEDIA_STATUS` messages have arrived on this connection.
	 *
	 * Only [awaitLoadAck] reads it, and only as a before-and-after comparison —
	 * the value itself means nothing, and it is deliberately not reset by
	 * [teardown], since a wrap or a stale figure cannot make two reads taken
	 * seconds apart look equal.
	 */
	private val mediaMessages = MutableStateFlow(0)

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

	/**
	 * The parameters of the last LOAD, so a retry can repeat it verbatim — and
	 * so the UI can say what the receiver is playing and where it is fetching
	 * it from, neither of which is recoverable from a `MEDIA_STATUS`: the
	 * receiver reports no codec and no bitrate, and the `contentType` it echoes
	 * is only what we put in the LOAD.
	 *
	 * Null between sessions, which is what makes the info dialog fall back to
	 * describing local playback rather than keeping a stale route on screen.
	 * A [MutableStateFlow] is already thread-safe, so the `@Volatile` this
	 * replaced is not missing.
	 */
	private val _loaded = MutableStateFlow<CastMedia?>(null)
	val loaded: StateFlow<CastMedia?> = _loaded.asStateFlow()

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
		_loaded.value = null
	}

	// ── Commands ────────────────────────────────────────────────────────────

	fun load(media: CastMedia) {
		val gen = loadGen.incrementAndGet()
		// The route is logged beside the URL because the two together are what
		// distinguishes a routing problem from a relaying one, which is the
		// same reason CastBridge logs every request it serves.
		Log.i(
			TAG,
			"load gen=$gen route=${media.route} url=${media.url}" +
				" start=${media.startSeconds}",
		)

		// Reset the visible status for the new track, seeding the duration we
		// were told so the UI has one before the receiver reports its own, and
		// arm the retry against the session this LOAD is about to replace.
		retry.arm(_status.value.mediaSessionId)
		_status.value = CastStatus(duration = media.durationSeconds?.toFloat() ?: 0f)
		_loaded.value = media

		scope.launch(Dispatchers.IO) { sendLoad(media, gen) }
	}

	fun play() = mediaCommand("PLAY")

	fun pause() = mediaCommand("PAUSE")

	fun seek(seconds: Float) = mediaCommand("SEEK") { put("currentTime", seconds) }

	fun stopPlayback() = mediaCommand("STOP")

	/**
	 * Turns on the subtitle track at [index] in the loaded film's caption list,
	 * or turns subtitles off when it is null.
	 *
	 * An index rather than a trackId, because that is what the picker publishes
	 * and what [CastPlayer] numbered its `Tracks` with — resolving it here,
	 * against the very list the LOAD went out with, is what keeps the two
	 * numberings from being two numberings. The same reasoning as
	 * `VideoSurface.selectTextTrack` for the local player.
	 *
	 * Recorded on `_loaded` as well as sent, so an auto-retry replays the
	 * selection the viewer made rather than the one the film started with.
	 */
	fun selectCaption(index: Int?) {
		val media = _loaded.value ?: return
		val ids = index?.let { media.captions.getOrNull(it) }
			?.let { listOf(it.trackId) }
			.orEmpty()
		if (index != null && ids.isEmpty()) {
			Log.w(TAG, "caption $index is not in the loaded track list; ignored")
			return
		}
		_loaded.value = media.copy(activeTrackIds = ids)
		mediaCommand("EDIT_TRACKS_INFO") {
			putJsonArray("activeTrackIds") { ids.forEach { add(it) } }
		}
	}

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
				// Counted before it is parsed, and that order is the point:
				// parse() returns null for an empty `status` array, which is
				// what a freshly launched receiver holding no media answers
				// with. awaitLoadAck needs "the receiver is talking to us on
				// the media namespace", which is this, not "it reported a
				// playable state", which is the line below.
				mediaMessages.value += 1
				CastStatus.parse(message)?.let(::onMediaStatus)
			}

			"ERROR" -> Log.w(TAG, "rx ERROR: $message")
			"PONG" -> Unit
			else -> Log.i(TAG, "rx ${CastStatus.typeOf(message)}")
		}
	}

	private suspend fun onReceiverStatus(open: CastChannel, message: JsonObject) {
		val transport = CastStatus.transportIdOf(message, CastNs.DEFAULT_MEDIA_APP)
		if (transport == null) {
			// Only when the receiver actually enumerated what it is running
			// and ours was not among it. A RECEIVER_STATUS announcing a volume
			// change carries no `applications` array at all, and reading that
			// as "the app is gone" threw away a working transport mid-session
			// — which cost the *next* load the whole GET_STATUS-and-LAUNCH
			// path, the slow route this timing bug lives on.
			if (CastStatus.listsApplications(message)) transportId.value = null
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
		// and must be carried forward rather than collapsing to zero. The
		// active subtitle tracks are stated on the same terms and carry forward
		// for the same reason — see CastStatus.activeTrackIds.
		val previous = _status.value
		val merged = fresh.copy(
			duration = if (fresh.duration == 0f) previous.duration else fresh.duration,
			activeTrackIds = fresh.activeTrackIds ?: previous.activeTrackIds,
		)
		_status.value = merged

		if (retry.onStatus(merged)) {
			val media = _loaded.value ?: return
			val gen = loadGen.get()
			Log.w(TAG, "auto-retry LOAD (gen=$gen) — receiver went IDLE/ERROR")
			scope.launch(Dispatchers.IO) { sendLoad(media, gen) }
		}
	}

	private suspend fun sendLoad(media: CastMedia, gen: Int) {
		if (loadGen.get() != gen) return
		// Said out loud. This was the one abandon in the sequence that logged
		// nothing at all, so a LOAD lost here left no trace anywhere — the
		// player simply never started and there was no line to search for.
		val open = awaitChannel() ?: run {
			Log.w(TAG, "no control channel after ${CHANNEL_WAIT_MS}ms; LOAD abandoned")
			reportReceiverFailure()
			return
		}
		if (loadGen.get() != gen) return

		val transport = ensureTransport(open) ?: run {
			Log.w(TAG, "no transportId from receiver; LOAD abandoned")
			reportReceiverFailure()
			return
		}
		if (loadGen.get() != gen) return

		val payload = loadPayload(media)
		Log.i(TAG, "LOAD $payload")
		open.send(CastNs.MEDIA, transport, payload)
		awaitLoadAck(open, transport, media, gen)
	}

	/**
	 * One LOAD message.
	 *
	 * Built by a function rather than inline because it is sent twice: once by
	 * [sendLoad] and again by [awaitLoadAck] if the receiver never answered.
	 * **The second must not be a byte-for-byte copy of the first** — a receiver
	 * correlates its replies by `requestId`, so re-sending one it has already
	 * seen is a worse thing to do than sending nothing.
	 */
	private fun loadPayload(media: CastMedia): JsonObject =
		buildJsonObject {
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
				if (media.captions.isNotEmpty()) {
					putJsonArray("tracks") {
						media.captions.forEach { caption ->
							addJsonObject {
								put("trackId", caption.trackId)
								put("type", "TEXT")
								put("subtype", "SUBTITLES")
								put("trackContentId", caption.url)
								put("trackContentType", "text/vtt")
								put("language", caption.language)
								put("name", caption.label)
							}
						}
					}
				}
			})
			if (media.activeTrackIds.isNotEmpty()) {
				putJsonArray("activeTrackIds") {
					media.activeTrackIds.forEach { add(it) }
				}
			}
		}

	/**
	 * Sends the LOAD once more if the receiver never acknowledged the first.
	 *
	 * **The gap `LoadRetryWatcher` is structurally unable to see.** That class
	 * decides from `MEDIA_STATUS` pushes, and the failure here produces none: a
	 * receiver that has published its transport but is not yet consuming the
	 * media namespace drops the LOAD on the floor. There is no ack to wait for
	 * — `send` reports only that the bytes left this phone — so elapsed time is
	 * the only evidence there is.
	 *
	 * Three rules keep it from becoming a source of its own problems:
	 *
	 *  * **Once, never a loop.** What this recovers from is a receiver that was
	 *    a moment too early. If the second is ignored too then something else
	 *    is wrong, and repeating would bury it.
	 *  * **Generation-checked**, like the three checks in [sendLoad] above, so a
	 *    track the user chose in the meantime always wins.
	 *  * **Judged on the message, not on a parsed status.** `CastStatus.parse`
	 *    returns null for an empty `status` array, which is exactly what a
	 *    freshly launched receiver with no media answers a `GET_STATUS` with —
	 *    so counting parsed statuses would call a healthy receiver silent and
	 *    re-send against it. [mediaMessages] counts arrivals instead.
	 */
	private suspend fun awaitLoadAck(
		open: CastChannel,
		transport: String,
		media: CastMedia,
		gen: Int,
	) {
		val before = mediaMessages.value
		delay(LOAD_ACK_WAIT_MS)
		if (loadGen.get() != gen) return
		if (mediaMessages.value != before) return
		if (channel.value !== open) return

		val again = loadPayload(media)
		Log.w(TAG, "no MEDIA_STATUS ${LOAD_ACK_WAIT_MS}ms after LOAD; sending it once more")
		Log.i(TAG, "LOAD $again")
		open.send(CastNs.MEDIA, transport, again)
	}

	/**
	 * The same words for both ways a LOAD can be given up on, and deliberately
	 * about the receiver rather than the file: nothing here is a judgement on
	 * what was being cast, and saying so would send the next person to look at
	 * the wrong thing.
	 */
	private fun reportReceiverFailure() = report(
		"The television did not finish starting up, so nothing was sent to it. " +
			"Try again."
	)

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

		/**
		 * How long a LOAD waits for the TLS channel to exist.
		 *
		 * **It must exceed the time opening one can take**, which was not true
		 * of the 5 s it started at: `CastChannel.open` spends up to
		 * `CONNECT_TIMEOUT_MS` (5 s) on the Wi-Fi-bound connect, then
		 * `FALLBACK_TIMEOUT_MS` (3 s) on the unbound one, and only then runs a
		 * handshake under another 5 s `soTimeout` — about thirteen seconds
		 * worst case. A ceiling below the work it bounds is not a timeout, it
		 * is a race, and losing it abandoned the LOAD.
		 */
		const val CHANNEL_WAIT_MS = 20_000L

		/**
		 * How long to wait for a `RECEIVER_STATUS` naming our app *before*
		 * launching it.
		 *
		 * Not about the launch at all — it is what decides whether an already
		 * running receiver is joined or torn down. `ensureTransport` sends
		 * LAUNCH when this expires, and LAUNCH against a running app recreates
		 * it, losing the session. 1.5 s is easily too short for a television
		 * that is waking up to answer a `GET_STATUS`.
		 */
		const val STATUS_WAIT_MS = 4_000L

		/**
		 * How long to wait for the receiver app after LAUNCH.
		 *
		 * **This is a television changing HDMI input and cold-starting a web
		 * app, not a network round trip.** Measured at 10–20 s on the
		 * reference device, so the 10 s this started at expired first perhaps
		 * half the time: the LOAD was abandoned, the receiver finished
		 * launching a few seconds later, and the set sat on the Chromecast
		 * idle backdrop with a live control channel and nothing loaded. A
		 * second attempt always worked, because `ensureTransport` then found
		 * the transport already cached.
		 *
		 * Waiting longer costs nothing: `awaitTransport` is event-driven and
		 * returns the instant `onReceiverStatus` publishes the transport, and
		 * a newer load supersedes this one through `loadGen`.
		 */
		const val LAUNCH_WAIT_MS = 45_000L

		/**
		 * How long to give a LOAD that was *sent* before sending it once more.
		 *
		 * The gap `LoadRetryWatcher` cannot cover: it is fed by `MEDIA_STATUS`
		 * pushes, so it sees nothing at all when the receiver drops the LOAD —
		 * which it does when the app has published its transport but is not yet
		 * consuming the media namespace. There is no ack to wait on, so a timer
		 * is the only evidence available.
		 */
		const val LOAD_ACK_WAIT_MS = 8_000L

		const val STOP_GRACE_MS = 300L
	}
}

/** Lets [CastSession.mediaCommand] take extra fields without a builder class. */
private typealias JsonObjectBuilderScope =
	kotlinx.serialization.json.JsonObjectBuilder.() -> Unit
