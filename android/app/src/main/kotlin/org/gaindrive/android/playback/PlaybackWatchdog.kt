package org.gaindrive.android.playback

import androidx.media3.common.Player
import androidx.media3.exoplayer.ExoPlayer
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Job
import kotlinx.coroutines.delay
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.asStateFlow
import kotlinx.coroutines.launch
import javax.inject.Inject
import javax.inject.Singleton

/**
 * Stops playback that has stopped making progress, and says so.
 *
 * There is a class of failure where nothing fails. The server keeps sending,
 * the player keeps loading, no read times out and no `PlaybackException` is
 * ever raised — but the clock does not advance, so the buffering spinner stays
 * up for good. HLS segments carrying the wrong timestamps did exactly this. The
 * loading machinery cannot notice, because from its point of view everything is
 * working; only the position gives it away.
 *
 * Stopping the player is what un-wedges it: the stall cannot clear itself,
 * and only the stop makes the retry the message offers start from a clean
 * player. (This stop once also carried a second job — `onTaskRemoved` used to
 * end the service only when the player was not `playWhenReady`, so a wedged
 * player survived swiping the app away until something stopped it. A swipe
 * now always pauses and ends the service, so that job is gone.)
 *
 * Attached to the [ExoPlayer] directly, like [VideoSurface] and for the same
 * reason — it has to work with nothing bound to the session, which is precisely
 * the case that got stuck.
 *
 * This is a safety net, not a diagnosis. It says only that something is wrong,
 * and the user is offered a retry.
 */
@Singleton
class PlaybackWatchdog @Inject constructor(
	private val scope: CoroutineScope,
) {

	private val _message = MutableStateFlow<String?>(null)

	/** Set when a stall was caught, until the UI clears it. */
	val message: StateFlow<String?> = _message.asStateFlow()

	fun clear() {
		_message.value = null
	}

	private var player: ExoPlayer? = null
	private var countdown: Job? = null

	private val listener = object : Player.Listener {
		override fun onEvents(player: Player, events: Player.Events) {
			if (player.playbackState == Player.STATE_BUFFERING && player.playWhenReady) {
				arm(player)
			} else {
				disarm()
			}
		}
	}

	/** Called by [PlaybackService] with its player, and with null on destroy. */
	fun registerPlayer(player: ExoPlayer?) {
		this.player?.removeListener(listener)
		disarm()
		this.player = player
		player?.addListener(listener)
	}

	/**
	 * Starts counting, or leaves an existing count alone.
	 *
	 * Not restarted on every event: `onEvents` fires repeatedly throughout a
	 * stall, and re-arming each time would push the deadline out forever. The
	 * timer therefore measures one *continuous* stretch of buffering, and any
	 * other state disarms it — so a slow link that manages a second of playback
	 * between stalls is never touched.
	 */
	private fun arm(player: Player) {
		if (countdown?.isActive == true) return
		countdown = scope.launch {
			val positionAtStart = player.currentPosition
			delay(timeoutFor(player))

			// Re-checked rather than assumed: disarm cancels this job, but the
			// cancellation and the resumption can race, and killing playback
			// that has just recovered would be the worse failure.
			if (player.playbackState != Player.STATE_BUFFERING) return@launch
			if (!player.playWhenReady) return@launch
			if (player.currentPosition != positionAtStart) return@launch

			// Published before stopping, not after: stop() reaches the listener
			// synchronously on this dispatcher, which disarms and so cancels
			// the job this line is running in. A non-suspending assignment
			// would survive that, but only by accident.
			_message.value = "Playback stalled and was stopped."
			player.stop()
		}
	}

	private fun disarm() {
		countdown?.cancel()
		countdown = null
	}

	/**
	 * How long this item is allowed to make no progress for.
	 *
	 * A film is the exception, whether it is being watched or played for its
	 * soundtrack. gaindrive's transcode cache is blocking — it runs ffmpeg over
	 * the whole source and sends nothing at all until the file is complete — so
	 * a remux or an audio extraction of a multi-gigabyte file is minutes of
	 * buffering in which no byte arrives and the position cannot move. That is
	 * indistinguishable from a wedge by every signal this class has, and half a
	 * minute of it is normal rather than broken.
	 *
	 * The real answer to that wait is `TranscodePrewarmer`, which moves it off
	 * the tap for everything but the first track. This is what keeps the safety
	 * net from firing on the first one.
	 */
	private fun timeoutFor(player: Player): Long {
		val item = player.currentMediaItem ?: return STALL_TIMEOUT_MS
		return if (item.isVideo() || item.isAudioOnlyVideo()) BUILD_TIMEOUT_MS
		else STALL_TIMEOUT_MS
	}

	private companion object {
		/**
		 * Long enough that a slow link is not mistaken for a broken one — the
		 * player starts on a couple of seconds of buffer, so half a minute
		 * without reaching that is not a bandwidth problem — and short enough
		 * that nobody sits watching a spinner wondering.
		 */
		const val STALL_TIMEOUT_MS = 30_000L

		/**
		 * Sized for the slowest build worth waiting for: a several-hour source
		 * has to be read end to end before a byte of it is sent. Below the
		 * `MediaHttp` read timeout on purpose, so a server that really has
		 * given up is still reported by the HTTP layer, which knows why.
		 */
		const val BUILD_TIMEOUT_MS = 5 * 60_000L
	}
}
