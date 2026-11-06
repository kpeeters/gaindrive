package org.gaindrive.android.playback

import android.os.Looper
import androidx.media3.common.C
import androidx.media3.common.Player
import androidx.media3.exoplayer.ExoPlayer
import androidx.media3.exoplayer.PlayerMessage
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Job
import kotlinx.coroutines.launch
import org.gaindrive.android.data.Connectivity
import org.gaindrive.android.data.cache.PinRepository
import javax.inject.Inject
import javax.inject.Singleton

/**
 * Offline, keeps a film's soundtrack inside the stretches of it that are
 * stored.
 *
 * A pinned chapter plays from the cache like anything else, but the film it
 * belongs to is one item, and nothing stops the player running on past the
 * chapter's end into segments that are not here. With no network it would sit
 * buffering until the load gave up and failed the item. So, offline, the end
 * of the stored stretch being played is marked on the player, and reaching it
 * moves on to the next stored stretch of the same film, or to the next item
 * when there is none. Starting or seeking into a part that is not stored moves
 * on in the same way.
 *
 * Online it does nothing: what is not stored streams.
 *
 * Attached to the [ExoPlayer] directly, like [PlaybackWatchdog], so it works
 * with nothing bound to the session.
 */
@Singleton
class HeldRangeGuard @Inject constructor(
	private val pins: PinRepository,
	private val connectivity: Connectivity,
	private val scope: CoroutineScope,
) {

	private var player: ExoPlayer? = null
	private var marker: PlayerMessage? = null
	private var watch: Job? = null

	private val listener = object : Player.Listener {
		override fun onEvents(player: Player, events: Player.Events) {
			if (events.containsAny(
					Player.EVENT_MEDIA_ITEM_TRANSITION,
					Player.EVENT_POSITION_DISCONTINUITY,
					Player.EVENT_PLAYBACK_STATE_CHANGED,
				)
			) {
				rearm()
			}
		}
	}

	/** Called by [PlaybackService] with its player, and with null on destroy. */
	fun registerPlayer(player: ExoPlayer?) {
		this.player?.removeListener(listener)
		watch?.cancel()
		disarm()
		this.player = player
		if (player == null) return
		player.addListener(listener)
		// Losing the network mid-film is the case that matters most, and no
		// player event announces it.
		watch = scope.launch { connectivity.online.collect { rearm() } }
	}

	private fun rearm() {
		disarm()
		val player = player ?: return
		if (connectivity.online.value) return
		if (player.playbackState == Player.STATE_IDLE || player.playbackState == Player.STATE_ENDED) {
			return
		}
		val item = player.currentMediaItem ?: return
		if (!item.isAudioOnlyVideo()) return
		val film = item.itemRef() ?: return

		val held = merged(pins.heldRanges(film))
		if (held.firstOrNull()?.last == Long.MAX_VALUE) return

		val at = player.currentPosition
		val current = held.firstOrNull { at >= it.first - SLACK_MS && at <= it.last }
		if (current == null) {
			moveOn(player, held, at)
			return
		}
		val end = current.last + 1
		// A stretch running to the end of the film is left to end with it. Its
		// bound is whole seconds, from the browse duration, and the soundtrack
		// can run a fraction past that.
		val duration = player.duration
		if (duration != C.TIME_UNSET && end >= duration - END_SLACK_MS) return
		marker = player.createMessage { _, _ -> moveOn(player, held, end) }
			.setLooper(Looper.getMainLooper())
			.setPosition(player.currentMediaItemIndex, end)
			.send()
	}

	private fun disarm() {
		marker?.cancel()
		marker = null
	}

	/**
	 * To the next stored stretch starting at or after [from], else the next
	 * item. With neither, playback pauses where it is rather than running on
	 * into a stall.
	 */
	private fun moveOn(player: ExoPlayer, held: List<LongRange>, from: Long) {
		val next = held.firstOrNull { it.first >= from - SLACK_MS }
		when {
			next != null -> player.seekTo(next.first)
			player.hasNextMediaItem() -> player.seekToNextMediaItem()
			else -> player.pause()
		}
	}

	/**
	 * Adjacent and overlapping stretches as one, so two neighbouring pinned
	 * chapters play straight through rather than with a seek between them.
	 */
	private fun merged(ranges: List<LongRange>): List<LongRange> {
		val out = mutableListOf<LongRange>()
		ranges.sortedBy { it.first }.forEach { r ->
			val last = out.lastOrNull()
			if (last != null && r.first <= last.last + 1 + SLACK_MS) {
				out[out.lastIndex] = last.first..maxOf(last.last, r.last)
			} else {
				out += r
			}
		}
		return out
	}

	private companion object {
		/**
		 * Position jitter allowed around a stretch's start. A chapter row
		 * seeks to the chapter's start exactly, but the position the player
		 * then reports may land a frame either side of it.
		 */
		const val SLACK_MS = 250L

		/** More than the fraction of a second a whole-second duration drops. */
		const val END_SLACK_MS = 1_500L
	}
}
