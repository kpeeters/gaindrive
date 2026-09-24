package org.gaindrive.android.playback

import android.content.ComponentName
import android.content.Context
import android.view.SurfaceView
import androidx.media3.common.C
import androidx.media3.common.PlaybackException
import androidx.media3.common.Player
import androidx.media3.common.Tracks
import androidx.media3.session.MediaController
import androidx.media3.session.SessionToken
import androidx.media3.ui.SubtitleView
import com.google.common.util.concurrent.MoreExecutors
import dagger.hilt.android.qualifiers.ApplicationContext
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Job
import kotlinx.coroutines.delay
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.asStateFlow
import kotlinx.coroutines.flow.first
import kotlinx.coroutines.flow.update
import kotlinx.coroutines.isActive
import kotlinx.coroutines.launch
import org.gaindrive.android.data.CoverUrls
import org.gaindrive.android.data.LibraryRepository
import org.gaindrive.android.data.SettingsStore
import org.gaindrive.android.data.model.ItemRef
import org.gaindrive.android.data.model.Song
import org.gaindrive.android.playback.cast.CastSession
import org.gaindrive.android.playback.cast.CastUrls
import javax.inject.Inject
import javax.inject.Singleton

data class PlayerState(
	val current: NowPlaying? = null,
	val isPlaying: Boolean = false,
	/** The player has the track but is still filling its buffer. */
	val isBuffering: Boolean = false,
	/**
	 * A track the user asked for that is not audible yet.
	 *
	 * Set the instant a tap is handled, before the session is even bound, so
	 * the row that was tapped can say so. Everything between the tap and the
	 * first sound - binding the controller, resolving a stream URL, preparing,
	 * buffering - happens with no other visible change.
	 */
	val loadingRef: ItemRef? = null,
	val positionMs: Long = 0,
	val durationMs: Long = 0,
	val hasNext: Boolean = false,
	val hasPrevious: Boolean = false,
	val queue: List<NowPlaying> = emptyList(),
	val queueIndex: Int = 0,
	/**
	 * The subtitle tracks the current item offers, and which one is on. Empty
	 * for audio, and for a video whose captions could not be listed or whose
	 * source has none - a DVD always lands here, since its subtitles are
	 * bitmaps the server cannot turn into WebVTT.
	 */
	val textTracks: List<TextTrack> = emptyList(),
	/**
	 * The codec the local player is actually decoding, as media3 names a
	 * sample type - `audio/opus`, `audio/mpeg`, `audio/flac`.
	 *
	 * The request no longer settles this. A playback request declares the
	 * formats media3 takes as they stand, so a track asked for at Opus 160 may
	 * arrive as the original MP3, and [NowPlaying.quality] records only what
	 * was *asked for*. This is the answer from the decoder, which is the one
	 * party that cannot be wrong about it.
	 *
	 * Null until the tracks are known - they arrive shortly after playback
	 * starts rather than when the item is queued, the same reason
	 * [textTracks] is published state - and while casting, where nothing is
	 * being decoded here.
	 */
	val deliveredMime: String? = null,
	/**
	 * Why playback stopped, when it stopped for a reason worth explaining.
	 *
	 * Distinct from [isBuffering]: a stall that is still being waited on is
	 * buffering, and one the watchdog gave up on is this. The queue survives
	 * either way, so the only thing needed to recover is a retry.
	 */
	val error: String? = null,
) {
	/** The bar and sheet only exist once something has been queued. */
	val isActive: Boolean get() = current != null

	/** Whether what is playing wants a picture. */
	val isVideo: Boolean get() = current?.isVideo == true

	/**
	 * Whether the stream the server would send is seekable by byte range -
	 * true for audio, and for the video tiers that arrive as a real MP4.
	 *
	 * No longer what decides whether the cast button is drawn: a film the
	 * server can only re-encode is cast as HLS, so the question there is
	 * whether the *receiver* can reach the server, which
	 * [org.gaindrive.android.playback.cast.CastUrls.videoIsCastable] answers
	 * and no composable can.
	 */
	val nativeSeek: Boolean get() = current?.nativeSeek == true

	/** How [ref] should render in a track listing. */
	fun trackStateOf(ref: ItemRef): TrackState = when {
		loadingRef == ref -> TrackState.LOADING
		current?.ref != ref -> TrackState.IDLE
		isBuffering -> TrackState.LOADING
		else -> TrackState.CURRENT
	}
}

/** How a row in a track listing relates to the player. */
enum class TrackState { IDLE, LOADING, CURRENT }

/**
 * One selectable subtitle track.
 *
 * [index] is a position in [PlayerState.textTracks], not anything the server
 * issued: the picker's job is to name a track the player already knows about,
 * and going back through caption ids would mean re-deriving a mapping the
 * player is holding.
 */
data class TextTrack(val index: Int, val label: String, val selected: Boolean)

/**
 * The UI's only route to playback.
 *
 * Deliberately a [MediaController] rather than the [androidx.media3.exoplayer.ExoPlayer]
 * itself: the controller does not care what the session is driving, which is
 * what lets a Chromecast player be swapped in later without the UI noticing.
 */
@Singleton
class PlayerConnection @Inject constructor(
	@ApplicationContext private val context: Context,
	private val library: LibraryRepository,
	private val videoSurface: VideoSurface,
	private val watchdog: PlaybackWatchdog,
	private val castSession: CastSession,
	// For videoIsCastable() alone: whether a receiver could fetch a film the
	// server can only re-encode. The rule lives there so the refusal here and
	// the URL builder cannot disagree.
	private val castUrls: CastUrls,
	private val settings: SettingsStore,
	private val scope: CoroutineScope,
) {

	private val _state = MutableStateFlow(PlayerState())
	val state: StateFlow<PlayerState> = _state.asStateFlow()

	private val _message = MutableStateFlow<String?>(null)

	/**
	 * Something the user asked for that could not be done, in words. Held
	 * rather than thrown because the refusal has to outlive whatever sheet or
	 * row the tap came from; the shell shows it and calls [consumeMessage].
	 *
	 * Not [PlayerState.error], which is a different thing: this is a request
	 * that was declined and left nothing running, so it is shown once and
	 * dismissed. That one is playback having stopped, and stays on screen until
	 * it is retried or dismissed.
	 */
	val message: StateFlow<String?> = _message.asStateFlow()

	fun consumeMessage() {
		_message.value = null
	}

	/**
	 * The picture's shape, once the decoder has reported it. Null until then;
	 * the screen falls back to the figure the server gave for the entry.
	 */
	val videoAspectRatio: StateFlow<Float?> = videoSurface.aspectRatio

	private var controller: MediaController? = null
	private var ticker: Job? = null

	/** What the user last asked to hear, until it is actually audible. */
	private var pendingRef: ItemRef? = null
	private var pendingTimeout: Job? = null

	/**
	 * Boundary between hand-picked queue entries and the automatic tail. The
	 * rules live in [QueueBoundary]; this just holds the current value.
	 *
	 * Held here rather than in the service because every queue mutation goes
	 * through this class. The system's media notification only skips and seeks,
	 * so it cannot move the boundary behind our back.
	 */
	private var autoFrom = QueueBoundary.EMPTY

	private val listener = object : Player.Listener {
		override fun onPlayerError(error: PlaybackException) {
			// A failed load must not leave a row spinning forever.
			clearPending()
			publish()
		}

		override fun onEvents(player: Player, events: Player.Events) {
			publish()
			// Only poll while something is actually moving; a paused player's
			// position does not change, and a timer that runs anyway is a
			// battery cost for nothing.
			if (player.isPlaying) startTicker() else stopTicker()
		}
	}

	// Last, and deliberately: the collector runs eagerly on Main.immediate and
	// a StateFlow replays at once, so publish() is called before this
	// constructor returns - with everything above it already initialised.
	init {
		// The watchdog fires from the service, on no player event this class
		// would otherwise hear about, so its message has to push a republish
		// rather than wait for one.
		scope.launch { watchdog.message.collect { publish() } }
		// A refusal raised on the cast load worker, where there is no tap to
		// return it to. Forwarded into the same field the enqueue refusals
		// use, so the shell has one thing to show and one thing to clear.
		scope.launch {
			castSession.message.collect { text ->
				if (text != null) {
					_message.value = text
					castSession.consumeMessage()
				}
			}
		}
	}

	/** Idempotent: safe to call from every screen that needs the player. */
	fun connect() {
		if (controller != null) return
		val token = SessionToken(context, ComponentName(context, PlaybackService::class.java))
		val future = MediaController.Builder(context, token)
			// The service releases its session when the task is swiped away,
			// and this singleton usually outlives that - the process survives
			// the swipe. A dead controller kept here would make every later
			// connect() return early on it, so it is dropped the moment the
			// session goes; PlayerViewModel calls connect() again on reopen.
			.setListener(object : MediaController.Listener {
				override fun onDisconnected(disconnected: MediaController) {
					disconnected.release()
					if (controller !== disconnected) return
					controller = null
					stopTicker()
					clearPending()
					autoFrom = QueueBoundary.EMPTY
					// As in stop(): the watchdog outlives the service too, and
					// its held message would republish an explanation for a
					// queue that no longer exists.
					watchdog.clear()
					_state.value = PlayerState()
				}
			})
			.buildAsync()
		future.addListener(
			{
				controller = runCatching { future.get() }.getOrNull()?.also {
					it.addListener(listener)
					// A queue that outlived this process is treated as entirely
					// hand-picked. Assuming the opposite would let the next
					// enqueue silently delete a queue the user still wanted.
					autoFrom = QueueBoundary.adoptingExisting(it.mediaItemCount)
					publish()
					if (it.isPlaying) startTicker()
				}
			},
			MoreExecutors.directExecutor(),
		)
	}

	/**
	 * Whether [songs] can be played where playback is currently going.
	 *
	 * A video the server can only re-encode is cast as `hls.m3u8`, where
	 * seeking is picking a segment - so it plays and seeks on a receiver that
	 * fetches from the server itself. Through the bridge it cannot: the
	 * playlist's segment URIs are relative and the bridge's flat
	 * `/<token>/<key>` grammar 404s them. That is the only case left to refuse,
	 * and [CastUrls.videoIsCastable] is the one place the rule is written.
	 *
	 * The refusal used to cover every such video on both routes, which meant
	 * the common case - a receiver that can reach the server - was turned away
	 * for a limitation belonging to the rarer one.
	 *
	 * None of it applies when videos are being played for their soundtrack:
	 * what the receiver is offered then is an ordinary audio transcode, which
	 * has a length and byte ranges whatever the video codec was.
	 */
	private suspend fun refuseIfCasting(songs: List<Song>): Boolean {
		if (castSession.device.value == null) return false
		if (settings.videoAudioOnly.first()) return false
		val blocked = songs.any {
			it.isVideo && !castUrls.videoIsCastable(it.ref, it.nativeSeek)
		}
		if (!blocked) return false
		_message.value = "This video has to be converted as it plays, and this " +
			"receiver cannot reach the server to fetch it. Disconnect to watch " +
			"on this device."
		return true
	}

	/**
	 * Replaces the queue and starts at [startIndex], optionally partway in.
	 *
	 * [startPositionMs] is what a chapter marker asks for: play this recording,
	 * but from the song inside it that was tapped. Defaulted, so every caller
	 * that means "from the beginning" is unchanged.
	 *
	 * Handed to the player rather than followed by a `seekTo`, which both
	 * implementations already accept: `CastPlayer` forwards it into the LOAD it
	 * sends, and locally ExoPlayer applies it as it prepares. On a film the
	 * server can only re-encode, playback goes through `hls.m3u8` and the seek
	 * lands once the playlist has loaded, so the first frame is slower - that is
	 * the tier, not the offset.
	 */
	fun play(songs: List<Song>, startIndex: Int, startPositionMs: Long = 0L) = scope.launch {
		if (refuseIfCasting(songs)) return@launch
		markPending(songs.getOrNull(startIndex)?.ref)
		// Publish before awaiting the controller: on the very first tap the
		// session is not bound yet, and that wait is precisely the delay the
		// spinner exists to explain.
		publish()
		val controller = awaitController() ?: return@launch
		val covers: CoverUrls = library.coverUrls()
		val items = songs.map { it.toMediaItem(covers.url(it.coverArt, ARTWORK_PX)) }
		controller.setMediaItems(items, startIndex, startPositionMs)
		autoFrom = autoFrom.afterPlay(startIndex)
		controller.prepare()
		controller.play()
	}

	/**
	 * Appends a hand-picked track, discarding the automatic tail first.
	 *
	 * That truncation is the web client's rule and it is deliberate: without
	 * it, queueing a track behind a 15-track album buries it, which is never
	 * what "add to queue" is asked to mean.
	 */
	fun addToQueue(song: Song) = scope.launch {
		if (refuseIfCasting(listOf(song))) return@launch
		val controller = awaitController() ?: return@launch
		val covers = library.coverUrls()
		autoFrom.tailToDrop(controller.mediaItemCount)?.let { tail ->
			controller.removeMediaItems(tail.first, tail.last + 1)
		}
		controller.addMediaItem(song.toMediaItem(covers.url(song.coverArt, ARTWORK_PX)))
		autoFrom = autoFrom.afterAppend(controller.mediaItemCount)
		if (controller.mediaItemCount == 1) {
			controller.prepare()
			controller.play()
		}
	}

	/** Inserts a hand-picked track directly after the one playing. */
	fun playNext(song: Song) = scope.launch {
		if (refuseIfCasting(listOf(song))) return@launch
		val controller = awaitController() ?: return@launch
		val covers = library.coverUrls()
		val at = (controller.currentMediaItemIndex + 1).coerceIn(0, controller.mediaItemCount)
		controller.addMediaItem(at, song.toMediaItem(covers.url(song.coverArt, ARTWORK_PX)))
		autoFrom = autoFrom.afterInsert(at)
		if (controller.mediaItemCount == 1) {
			controller.prepare()
			controller.play()
		}
	}

	fun togglePlayPause() {
		val controller = controller ?: return
		if (controller.isPlaying) controller.pause() else controller.play()
	}

	fun pause() = controller?.pause()

	fun next() = controller?.seekToNextMediaItem()

	/**
	 * Restart the current track when more than [MAX_SEEK_TO_PREVIOUS_MS] in,
	 * otherwise go to the one before - the convention every physical transport
	 * and every music app on both platforms follows, and the reason "previous"
	 * pressed twice reaches the previous track.
	 *
	 * **This used to be `seekToPreviousMediaItem()`, which always goes back**,
	 * and the divergence went unnoticed because the two methods are named a
	 * character apart and neither reads as the odd one out. `web/app.js` and the
	 * iOS client have both had the threshold since they were written, so this
	 * app was the one that disagreed.
	 *
	 * **Written out rather than delegated to `seekToPrevious()`**, which is
	 * Media3's own version of exactly this rule and would otherwise be the right
	 * call. It differs in one case that matters: `getPreviousMediaItemIndex()`
	 * is `INDEX_UNSET` on the first item of a queue with repeat off, and
	 * `seekToPrevious()` returns immediately on that - so the button would do
	 * *nothing at all* on the first track of an album, at any position, where
	 * the other two clients restart it. Spelling the rule out is what makes the
	 * three agree, which is the whole point of changing it.
	 *
	 * Works the same while casting: `CastPlayer` is a `SimpleBasePlayer` whose
	 * `handleSeek` takes an index and a position, so both branches are ordinary
	 * seeks it already serves.
	 */
	fun previous() {
		val controller = controller ?: return
		if (controller.currentPosition > MAX_SEEK_TO_PREVIOUS_MS ||
			!controller.hasPreviousMediaItem()
		) {
			controller.seekTo(0)
		} else {
			controller.seekToPreviousMediaItem()
		}
	}

	fun seekTo(positionMs: Long) = controller?.seekTo(positionMs)

	/**
	 * Move [deltaMs] from wherever the player is now - the skip buttons over a
	 * picture.
	 *
	 * The position comes from the controller rather than from [PlayerState],
	 * whose own is up to [POSITION_POLL_MS] old: a press landing late in a tick
	 * would otherwise move nine and a half seconds, and a run of presses would
	 * drift differently every time.
	 *
	 * Clamped short of the end rather than to it, matching `playerSkip()` in
	 * `web/app.js`: a seek to the duration ends the item and starts the next
	 * one, and a forward skip is not a request to leave the film. A duration
	 * that is not known yet leaves the upper bound to the player.
	 *
	 * Works while casting for the reason [previous] does - an ordinary seek is
	 * something `CastPlayer` already serves.
	 */
	fun seekBy(deltaMs: Long) {
		val controller = controller ?: return
		val last = controller.duration
			.takeIf { it > 0 }
			?.let { (it - END_GUARD_MS).coerceAtLeast(0L) }
			?: Long.MAX_VALUE
		controller.seekTo((controller.currentPosition + deltaMs).coerceIn(0L, last))
	}

	/**
	 * Starts the stopped queue again from where it was.
	 *
	 * `stop()` keeps the media items and the current index, so preparing again
	 * is the whole recovery - there is nothing to rebuild and no position to
	 * restore by hand.
	 */
	fun retry() {
		val controller = controller ?: return
		watchdog.clear()
		controller.prepare()
		controller.play()
	}

	/** Dismisses the explanation without acting on it. */
	fun clearError() = watchdog.clear()

	// ── Video output ────────────────────────────────────────────────────────
	//
	// Delegated rather than done here: the surface has to reach the ExoPlayer
	// itself, and the reasons are all in VideoSurface. Routed through this
	// class anyway so the screen still talks to one thing.

	fun attachVideo(surface: SurfaceView, subtitles: SubtitleView) =
		videoSurface.attach(surface, subtitles)

	fun detachVideo(surface: SurfaceView) = videoSurface.detach(surface)

	/**
	 * Turns on the track at [index] in [PlayerState.textTracks], or turns
	 * subtitles off when it is null.
	 *
	 * Only the number crosses over. The groups behind it are published state
	 * and are what [textTracks] labels, but the `TrackGroup` objects a
	 * controller hands back are rebuilt from a bundle, and an override keyed on
	 * one of those never matches the player's own. See [VideoSurface].
	 *
	 * While casting there is no local selection to make: the receiver renders
	 * the captions and is told which one by `EDIT_TRACKS_INFO`. The index is
	 * the same either way, because `CastPlayer` publishes its `Tracks` in the
	 * order it numbers the tracks it sent.
	 */
	fun selectTextTrack(index: Int?) {
		if (castSession.device.value != null) castSession.selectCaption(index)
		else                                  videoSurface.selectTextTrack(index)
	}

	fun jumpTo(queueIndex: Int) {
		controller?.seekTo(queueIndex, 0L)
		controller?.play()
	}

	fun removeFromQueue(queueIndex: Int) {
		val controller = controller ?: return
		if (queueIndex in 0 until controller.mediaItemCount) {
			controller.removeMediaItem(queueIndex)
			autoFrom = autoFrom.afterRemove(queueIndex)
		}
	}

	fun stop() {
		controller?.run {
			clearMediaItems()
			stop()
		}
		autoFrom = QueueBoundary.EMPTY
		// Cleared at the source, not just in the snapshot below: the watchdog
		// holds the message, so the next publish would otherwise bring back an
		// explanation for a queue that no longer exists.
		watchdog.clear()
		_state.value = PlayerState()
	}

	private suspend fun awaitController(): MediaController? {
		connect()
		// The session binds asynchronously; a tap can land before it is ready.
		repeat(CONNECT_ATTEMPTS) {
			controller?.let { return it }
			delay(CONNECT_POLL_MS)
		}
		return controller
	}

	private fun publish() {
		val controller = controller
		if (controller == null) {
			// Nothing to report but the pending track - which is the whole
			// state there is before the session binds.
			_state.value = PlayerState(loadingRef = pendingRef, error = watchdog.message.value)
			return
		}

		// Resolved once the awaited track is genuinely audible. Buffering still
		// counts as pending, which is the whole point.
		val pending = pendingRef
		if (pending != null &&
			controller.currentMediaItem?.itemRef() == pending &&
			controller.isPlaying &&
			controller.playbackState == Player.STATE_READY
		) {
			clearPending()
		}
		val items = (0 until controller.mediaItemCount)
			.map { controller.getMediaItemAt(it).toNowPlaying() }
		_state.update {
			PlayerState(
				current = controller.currentMediaItem?.toNowPlaying(),
				isPlaying = controller.isPlaying,
				isBuffering = controller.playbackState == Player.STATE_BUFFERING,
				loadingRef = pendingRef,
				positionMs = controller.currentPosition.coerceAtLeast(0),
				durationMs = controller.duration.takeIf { d -> d > 0 } ?: 0,
				hasNext = controller.hasNextMediaItem(),
				hasPrevious = controller.hasPreviousMediaItem(),
				queue = items,
				queueIndex = controller.currentMediaItemIndex,
				textTracks = controller.textTracks(),
				deliveredMime = controller.deliveredAudioMime(),
				error = watchdog.message.value,
			)
		}
	}

	/**
	 * One entry per subtitle group the player has resolved. Side-loaded tracks
	 * only appear once the source has been prepared, so the picker fills in
	 * shortly after playback starts rather than at the moment the item is
	 * queued - which is why this is published state and not a one-shot query.
	 */
	private fun MediaController.textTracks(): List<TextTrack> =
		textGroups().mapIndexed { index, group ->
			val format = group.mediaTrackGroup.getFormat(0)
			TextTrack(
				index = index,
				label = format.label ?: format.language ?: "Track ${index + 1}",
				selected = group.isSelected,
			)
		}

	/**
	 * The subtitle groups, in the order the picker numbers them.
	 *
	 * [VideoSurface] applies [subtitleGroups] to the *player's* `currentTracks`
	 * to resolve that number back to a group. The two lists hold different
	 * objects - the controller's are rebuilt from a bundle - but they hold them
	 * in the same order, since one is a copy of the other, and the order is all
	 * an index needs.
	 */
	private fun MediaController.textGroups(): List<Tracks.Group> =
		currentTracks.subtitleGroups()

	/**
	 * The sample type of the audio track being decoded, or null before the
	 * player knows.
	 *
	 * Read here rather than derived from the request, because the request no
	 * longer decides: a playback URL declares what media3 takes as it stands,
	 * so the bytes may be the file on the server rather than the format asked
	 * for. `Format.sampleMimeType` is what the extractor settled on, which
	 * cannot disagree with what is being heard.
	 *
	 * The *selected* group only. A container can offer several audio tracks -
	 * a film played for its soundtrack is the case here - and the one being
	 * decoded is the answer; the others describe bytes nobody is reading.
	 */
	private fun MediaController.deliveredAudioMime(): String? =
		currentTracks.groups
			.firstOrNull { it.type == C.TRACK_TYPE_AUDIO && it.isSelected }
			?.mediaTrackGroup?.getFormat(0)?.sampleMimeType

	/**
	 * Marks [ref] as awaited, with a watchdog: a load that neither succeeds nor
	 * reports an error would otherwise spin indefinitely.
	 */
	private fun markPending(ref: ItemRef?) {
		pendingRef = ref
		pendingTimeout?.cancel()
		if (ref == null) return
		pendingTimeout = scope.launch {
			delay(PENDING_TIMEOUT_MS)
			if (pendingRef == ref) {
				pendingRef = null
				publish()
			}
		}
	}

	private fun clearPending() {
		pendingRef = null
		pendingTimeout?.cancel()
		pendingTimeout = null
	}

	private fun startTicker() {
		if (ticker?.isActive == true) return
		ticker = scope.launch {
			while (isActive) {
				publish()
				delay(POSITION_POLL_MS)
			}
		}
	}

	private fun stopTicker() {
		ticker?.cancel()
		ticker = null
	}

	private companion object {
		const val POSITION_POLL_MS = 500L
		const val CONNECT_ATTEMPTS = 40
		const val CONNECT_POLL_MS = 50L
		/** Long enough for a slow link, short enough not to look stuck. */
		const val PENDING_TIMEOUT_MS = 30_000L

		/**
		 * How far into a track "previous" stops meaning "go back" and starts
		 * meaning "start this one again".
		 *
		 * Three seconds, matching `web/app.js` and the iOS client - and, as it
		 * happens, Media3's own `C.DEFAULT_MAX_SEEK_TO_PREVIOUS_POSITION_MS`,
		 * which is what `seekToPrevious()` would have used. The same number as
		 * [org.gaindrive.android.data.model.CHAPTER_RESTART_MS], for the same
		 * reason rather than by coincidence: a chapter marker is a track
		 * boundary inside a file, and the two controls must not disagree about
		 * how long "just started" lasts.
		 */
		const val MAX_SEEK_TO_PREVIOUS_MS = 3_000L

		/**
		 * How far short of the end [seekBy] is allowed to land. One second, as
		 * in `web/app.js`, which stops a forward skip near the end from
		 * finishing the item and starting the next one.
		 */
		const val END_GUARD_MS = 1_000L
	}
}
