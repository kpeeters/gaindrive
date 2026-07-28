package org.gaindrive.android.ui.player

import androidx.lifecycle.ViewModel
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

	init {
		// Binding early means the bar can restore an already-playing session
		// when the app is reopened, rather than appearing only on the next tap.
		player.connect()
	}

	fun play(songs: List<Song>, startIndex: Int) {
		player.play(songs, startIndex)
	}

	fun addToQueue(song: Song) {
		player.addToQueue(song)
	}

	fun togglePlayPause() = player.togglePlayPause()
	fun next() { player.next() }
	fun previous() { player.previous() }
	fun seekTo(positionMs: Long) { player.seekTo(positionMs) }
	fun jumpTo(index: Int) = player.jumpTo(index)
}
