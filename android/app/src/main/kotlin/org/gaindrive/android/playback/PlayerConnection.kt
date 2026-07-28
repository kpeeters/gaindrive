package org.gaindrive.android.playback

import android.content.ComponentName
import android.content.Context
import androidx.media3.common.Player
import androidx.media3.session.MediaController
import androidx.media3.session.SessionToken
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
import org.gaindrive.android.data.model.Song
import javax.inject.Inject
import javax.inject.Singleton

data class PlayerState(
	val current: NowPlaying? = null,
	val isPlaying: Boolean = false,
	val positionMs: Long = 0,
	val durationMs: Long = 0,
	val hasNext: Boolean = false,
	val hasPrevious: Boolean = false,
	val queue: List<NowPlaying> = emptyList(),
	val queueIndex: Int = 0,
) {
	/** The bar and sheet only exist once something has been queued. */
	val isActive: Boolean get() = current != null
}

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
	private val scope: CoroutineScope,
) {

	private val _state = MutableStateFlow(PlayerState())
	val state: StateFlow<PlayerState> = _state.asStateFlow()

	private var controller: MediaController? = null
	private var ticker: Job? = null

	private val listener = object : Player.Listener {
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
					publish()
					if (it.isPlaying) startTicker()
				}
			},
			MoreExecutors.directExecutor(),
		)
	}

	/** Replaces the queue and starts at [startIndex]. */
	fun play(songs: List<Song>, startIndex: Int) = scope.launch {
		val controller = awaitController() ?: return@launch
		val covers: CoverUrls = library.coverUrls()
		val items = songs.map { it.toMediaItem(covers.url(it.coverArt, ARTWORK_PX)) }
		controller.setMediaItems(items, startIndex, 0L)
		controller.prepare()
		controller.play()
	}

	/** Appends one track to the end of the queue. */
	fun addToQueue(song: Song) = scope.launch {
		val controller = awaitController() ?: return@launch
		val covers = library.coverUrls()
		controller.addMediaItem(song.toMediaItem(covers.url(song.coverArt, ARTWORK_PX)))
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

	fun jumpTo(queueIndex: Int) {
		controller?.seekTo(queueIndex, 0L)
		controller?.play()
	}

	fun removeFromQueue(queueIndex: Int) {
		val controller = controller ?: return
		if (queueIndex in 0 until controller.mediaItemCount) {
			controller.removeMediaItem(queueIndex)
		}
	}

	fun stop() {
		controller?.run {
			clearMediaItems()
			stop()
		}
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
		val controller = controller ?: return
		val items = (0 until controller.mediaItemCount)
			.map { controller.getMediaItemAt(it).toNowPlaying() }
		_state.update {
			PlayerState(
				current = controller.currentMediaItem?.toNowPlaying(),
				isPlaying = controller.isPlaying,
				positionMs = controller.currentPosition.coerceAtLeast(0),
				durationMs = controller.duration.takeIf { d -> d > 0 } ?: 0,
				hasNext = controller.hasNextMediaItem(),
				hasPrevious = controller.hasPreviousMediaItem(),
				queue = items,
				queueIndex = controller.currentMediaItemIndex,
			)
		}
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
	}
}
