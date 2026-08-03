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
import kotlinx.coroutines.flow.update
import kotlinx.coroutines.isActive
import kotlinx.coroutines.launch
import org.gaindrive.android.data.CoverUrls
import org.gaindrive.android.data.LibraryRepository
import org.gaindrive.android.data.model.ItemRef
import org.gaindrive.android.data.model.Song
import org.gaindrive.android.playback.cast.CastSession
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
	 * first sound — binding the controller, resolving a stream URL, preparing,
	 * buffering — happens with no other visible change.
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
	 * source has none — a DVD always lands here, since its subtitles are
	 * bitmaps the server cannot turn into WebVTT.
	 */
	val textTracks: List<TextTrack> = emptyList(),
) {
	/** The bar and sheet only exist once something has been queued. */
	val isActive: Boolean get() = current != null

	/** Whether what is playing wants a picture. */
	val isVideo: Boolean get() = current?.isVideo == true

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
	private val castSession: CastSession,
	private val scope: CoroutineScope,
) {

	private val _state = MutableStateFlow(PlayerState())
	val state: StateFlow<PlayerState> = _state.asStateFlow()

	private val _message = MutableStateFlow<String?>(null)

	/**
	 * Something the user asked for that could not be done, in words. Held
	 * rather than thrown because the refusal has to outlive whatever sheet or
	 * row the tap came from; the shell shows it and calls [consumeMessage].
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

	/** Idempotent: safe to call from every screen that needs the player. */
	fun connect() {
		if (controller != null) return
		val token = SessionToken(context, ComponentName(context, PlaybackService::class.java))
		val future = MediaController.Builder(context, token).buildAsync()
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
	 * Video is not cast. The receiver would need a video MIME and a video
	 * metadata type, and the throttle in the server's stream path assumes a
	 * near-constant bitrate that VBR video does not have — so handing it a film
	 * fails minutes in rather than at the tap. Refusing here, in words, is the
	 * honest version of that.
	 */
	private fun refuseIfCasting(songs: List<Song>): Boolean {
		if (castSession.device.value == null || songs.none { it.isVideo }) return false
		_message.value = "Video cannot be cast. Disconnect to watch on this device."
		return true
	}

	/** Replaces the queue and starts at [startIndex]. */
	fun play(songs: List<Song>, startIndex: Int) = scope.launch {
		if (refuseIfCasting(songs)) return@launch
		markPending(songs.getOrNull(startIndex)?.ref)
		// Publish before awaiting the controller: on the very first tap the
		// session is not bound yet, and that wait is precisely the delay the
		// spinner exists to explain.
		publish()
		val controller = awaitController() ?: return@launch
		val covers: CoverUrls = library.coverUrls()
		val items = songs.map { it.toMediaItem(covers.url(it.coverArt, ARTWORK_PX)) }
		controller.setMediaItems(items, startIndex, 0L)
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

	fun next() = controller?.seekToNextMediaItem()
	fun previous() = controller?.seekToPreviousMediaItem()
	fun seekTo(positionMs: Long) = controller?.seekTo(positionMs)

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
	 * The groups are read from the controller — published state, always
	 * propagated — and the choice is applied to the player, where a refused
	 * session command cannot turn it into a silent no-op. See [VideoSurface].
	 */
	fun selectTextTrack(index: Int?) {
		val groups = controller?.textGroups().orEmpty()
		videoSurface.selectTextTrack(index?.let { groups.getOrNull(it) }?.mediaTrackGroup)
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
			// Nothing to report but the pending track — which is the whole
			// state there is before the session binds.
			_state.value = PlayerState(loadingRef = pendingRef)
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
			)
		}
	}

	/**
	 * One entry per subtitle group the player has resolved. Side-loaded tracks
	 * only appear once the source has been prepared, so the picker fills in
	 * shortly after playback starts rather than at the moment the item is
	 * queued — which is why this is published state and not a one-shot query.
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
	 * The subtitle groups, in the order the picker numbers them. Shared with
	 * [selectTextTrack] so an index means the same thing to both.
	 */
	private fun MediaController.textGroups(): List<Tracks.Group> =
		currentTracks.groups.filter { it.type == C.TRACK_TYPE_TEXT }

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
		/** Notification artwork; the system scales it down from here. */
		const val ARTWORK_PX = 512
		const val POSITION_POLL_MS = 500L
		const val CONNECT_ATTEMPTS = 40
		const val CONNECT_POLL_MS = 50L
		/** Long enough for a slow link, short enough not to look stuck. */
		const val PENDING_TIMEOUT_MS = 30_000L
	}
}
