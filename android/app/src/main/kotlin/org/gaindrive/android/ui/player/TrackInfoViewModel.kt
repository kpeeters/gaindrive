package org.gaindrive.android.ui.player

import androidx.lifecycle.ViewModel
import androidx.lifecycle.viewModelScope
import dagger.hilt.android.lifecycle.HiltViewModel
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.asStateFlow
import kotlinx.coroutines.launch
import org.gaindrive.android.data.ServerRegistry
import org.gaindrive.android.data.local.LocalLibrary
import org.gaindrive.android.data.model.ItemRef
import org.gaindrive.android.data.model.Song
import org.gaindrive.android.playback.cast.CastDevice
import org.gaindrive.android.playback.cast.CastMedia
import org.gaindrive.android.playback.cast.CastSession
import javax.inject.Inject

/**
 * What the track info dialog knows about the track it was opened on, beyond
 * what the player already publishes.
 *
 * [song] is null until the mirror answers, and stays null for a track that was
 * never stored there — the dialog then shows what the queue entry carries and
 * omits the rest, which is the same rule the web client's modal follows for a
 * missing field.
 */
data class TrackInfoState(
	val song: Song? = null,
	/** The name of the server this track came from, for the cast route line. */
	val serverName: String? = null,
)

/**
 * The dialog's view of one track.
 *
 * The library half is read from the offline mirror rather than from the server,
 * which is what the web client's `getSong` re-fetch does. It re-fetches to
 * refresh `transcodedSuffix`/`transcodedBitRate`, and this app needs neither:
 * the phone decides the quality itself, through `AudioQuality.cappedBy`, and
 * records the answer on the item. So a local read is both sufficient and the
 * only version that works with no connectivity — which is exactly the state a
 * downloaded library, and the cast route that serves it, exist for.
 */
@HiltViewModel
class TrackInfoViewModel @Inject constructor(
	private val local: LocalLibrary,
	private val registry: ServerRegistry,
	castSession: CastSession,
) : ViewModel() {

	private val _state = MutableStateFlow(TrackInfoState())
	val state: StateFlow<TrackInfoState> = _state.asStateFlow()

	/** What the receiver was last told to play, or null when not casting. */
	val loaded: StateFlow<CastMedia?> = castSession.loaded

	val device: StateFlow<CastDevice?> = castSession.device

	/**
	 * Cleared before the lookup rather than merged into it: the dialog is keyed
	 * on the current track, so a stale bitrate from the previous one would be
	 * on screen for as long as the read took.
	 */
	fun load(ref: ItemRef) {
		_state.value = TrackInfoState()
		viewModelScope.launch {
			_state.value = TrackInfoState(
				song = local.song(ref),
				serverName = registry.get(ref.server)?.name,
			)
		}
	}
}
