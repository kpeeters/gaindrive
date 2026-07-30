package org.gaindrive.android.ui.player

import androidx.lifecycle.ViewModel
import androidx.lifecycle.viewModelScope
import dagger.hilt.android.lifecycle.HiltViewModel
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.asStateFlow
import kotlinx.coroutines.launch
import org.gaindrive.android.data.LibraryRepository
import org.gaindrive.android.data.model.ItemRef
import org.gaindrive.android.data.model.Playlist
import org.gaindrive.android.data.model.ServerId
import org.gaindrive.android.net.runCatchingCancellable
import org.gaindrive.android.net.userMessage
import org.gaindrive.android.ui.Load
import javax.inject.Inject

/**
 * Backs the playlist picker inside [TrackActionsSheet].
 *
 * A playlist cannot hold a song from another server, so everything here is
 * scoped to the server that owns the track — including a new playlist, which is
 * created there without asking.
 */
@HiltViewModel
class AddToPlaylistViewModel @Inject constructor(
	private val library: LibraryRepository,
) : ViewModel() {

	private val _state = MutableStateFlow<Load<List<Playlist>>>(Load.Loading)
	val state: StateFlow<Load<List<Playlist>>> = _state.asStateFlow()

	/** An add or create in flight; the picker disables itself meanwhile. */
	private val _busy = MutableStateFlow(false)
	val busy: StateFlow<Boolean> = _busy.asStateFlow()

	private val _error = MutableStateFlow<String?>(null)
	val error: StateFlow<String?> = _error.asStateFlow()

	/** Raised on success so the sheet can close itself. */
	private val _done = MutableStateFlow(false)
	val done: StateFlow<Boolean> = _done.asStateFlow()

	private var loadedFor: ServerId? = null

	/** Idempotent per server: reopening the picker must not re-fetch. */
	fun load(server: ServerId) {
		if (loadedFor == server && _state.value is Load.Ready) return
		loadedFor = server
		_state.value = Load.Loading
		viewModelScope.launch {
			_state.value = runCatchingCancellable { library.playlistsOf(server) }.fold(
				onSuccess = { Load.Ready(it) },
				onFailure = { Load.Failed(it.userMessage()) },
			)
		}
	}

	fun reload() {
		loadedFor?.let {
			loadedFor = null
			load(it)
		}
	}

	fun add(playlist: ItemRef, songId: String) = perform("Could not add to playlist") {
		library.addToPlaylist(playlist, songId)
	}

	fun create(server: ServerId, name: String, songId: String) =
		perform("Could not create playlist") {
			library.createPlaylist(server, name.trim(), listOf(songId))
		}

	fun consumeDone() {
		_done.value = false
	}

	fun clearError() {
		_error.value = null
	}

	private fun perform(failure: String, block: suspend () -> Unit) {
		if (_busy.value) return
		_busy.value = true
		viewModelScope.launch {
			runCatchingCancellable { block() }.fold(
				onSuccess = {
					// The list just changed — drop the cached one so the next
					// track's picker shows the playlist that was created here.
					loadedFor = null
					// The repository's revision bump refreshes the playlists
					// screen; this only has to close the sheet.
					_done.value = true
				},
				onFailure = { _error.value = "$failure: ${it.userMessage()}" },
			)
			_busy.value = false
		}
	}
}
