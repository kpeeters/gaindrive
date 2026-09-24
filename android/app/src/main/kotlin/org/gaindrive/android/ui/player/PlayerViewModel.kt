package org.gaindrive.android.ui.player

import android.view.SurfaceView
import androidx.lifecycle.ViewModel
import androidx.media3.ui.SubtitleView
import dagger.hilt.android.lifecycle.HiltViewModel
import kotlinx.coroutines.flow.StateFlow
import org.gaindrive.android.data.model.Song
import org.gaindrive.android.playback.PlayerConnection
import org.gaindrive.android.playback.PlayerState
import javax.inject.Inject

/**
 * Thin wrapper over [PlayerConnection], which is a singleton because playback
 * outlives any screen. This exists only so composables can reach it the same
 * way they reach everything else.
 */
@HiltViewModel
class PlayerViewModel @Inject constructor(
	private val player: PlayerConnection,
) : ViewModel() {

	val state: StateFlow<PlayerState> = player.state

	val videoAspectRatio: StateFlow<Float?> = player.videoAspectRatio

	val message: StateFlow<String?> = player.message

	fun consumeMessage() = player.consumeMessage()

	init {
		// Binding early means the bar can restore an already-playing session
		// when the app is reopened, rather than appearing only on the next tap.
		player.connect()
	}

	fun play(songs: List<Song>, startIndex: Int, startPositionMs: Long = 0L) {
		player.play(songs, startIndex, startPositionMs)
	}

	fun addToQueue(song: Song) {
		player.addToQueue(song)
	}

	fun playNext(song: Song) {
		player.playNext(song)
	}

	fun removeFromQueue(index: Int) = player.removeFromQueue(index)

	fun togglePlayPause() = player.togglePlayPause()
	fun pause() { player.pause() }
	fun next() { player.next() }
	fun previous() { player.previous() }
	fun seekTo(positionMs: Long) { player.seekTo(positionMs) }
	fun seekBy(deltaMs: Long) { player.seekBy(deltaMs) }
	fun jumpTo(index: Int) = player.jumpTo(index)

	fun attachVideo(surface: SurfaceView, subtitles: SubtitleView) =
		player.attachVideo(surface, subtitles)

	fun detachVideo(surface: SurfaceView) = player.detachVideo(surface)

	fun selectTextTrack(index: Int?) = player.selectTextTrack(index)

	fun retry() = player.retry()
	fun clearError() = player.clearError()
}
