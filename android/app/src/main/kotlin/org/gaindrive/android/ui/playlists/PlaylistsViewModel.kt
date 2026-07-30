package org.gaindrive.android.ui.playlists

import androidx.lifecycle.ViewModel
import androidx.lifecycle.viewModelScope
import dagger.hilt.android.lifecycle.HiltViewModel
import kotlinx.coroutines.Job
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.SharingStarted
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.asStateFlow
import kotlinx.coroutines.flow.combine
import kotlinx.coroutines.flow.distinctUntilChanged
import kotlinx.coroutines.flow.stateIn
import kotlinx.coroutines.launch
import org.gaindrive.android.data.LibraryRepository
import org.gaindrive.android.data.ServerSelection
import org.gaindrive.android.data.model.Playlist
import org.gaindrive.android.data.model.ServerConfig
import org.gaindrive.android.data.model.ServerId
import org.gaindrive.android.net.runCatchingCancellable
import org.gaindrive.android.net.userMessage
import org.gaindrive.android.ui.Load
import javax.inject.Inject

@HiltViewModel
class PlaylistsViewModel @Inject constructor(
	private val library: LibraryRepository,
	private val selection: ServerSelection,
) : ViewModel() {

	private val _state = MutableStateFlow<Load<List<Playlist>>>(Load.Loading)
	val state: StateFlow<Load<List<Playlist>>> = _state.asStateFlow()

	/** True only for a user-initiated pull, which drives the pull indicator. */
	private val _isRefreshing = MutableStateFlow(false)
	val isRefreshing: StateFlow<Boolean> = _isRefreshing.asStateFlow()

	/** A failed edit, shown once in a snackbar. */
	private val _error = MutableStateFlow<String?>(null)
	val error: StateFlow<String?> = _error.asStateFlow()

	private var server: ServerConfig? = null
	private var loadJob: Job? = null

	init {
		viewModelScope.launch {
			combine(
				selection.current.distinctUntilChanged(),
				// Re-reads after a create or delete anywhere in the app.
				library.playlistRevision,
			) { selected, _ -> selected }.collect { selected ->
				// A different server is a different set of playlists, so the old
				// list must go. A revision bump is the same list changed, and
				// blanking it there would flash the whole screen on every edit.
				val switched = selected?.id != server?.id
				server = selected
				startLoad(clearFirst = switched)
			}
		}
	}

	/** Retry after a failure: there is nothing worth keeping on screen. */
	fun load() = startLoad(clearFirst = true)

	fun refresh() {
		_isRefreshing.value = true
		startLoad(clearFirst = false)
	}

	fun delete(playlist: Playlist) {
		viewModelScope.launch {
			// No optimistic removal: the reload the revision bump triggers is
			// what takes the row away, so the list can never claim a deletion
			// the server refused.
			runCatchingCancellable { library.deletePlaylist(playlist.ref) }
				.onFailure { _error.value = "Could not delete: ${it.userMessage()}" }
		}
	}

	fun clearError() {
		_error.value = null
	}

	private fun startLoad(clearFirst: Boolean) {
		loadJob?.cancel()
		loadJob = viewModelScope.launch {
			val current = server
			if (current == null) {
				_state.value = Load.Failed("No server configured. Add one in Settings.")
				_isRefreshing.value = false
				return@launch
			}
			if (clearFirst) _state.value = Load.Loading

			_state.value = runCatchingCancellable { library.playlists(current.id) }
				.fold(
					onSuccess = { Load.Ready(it) },
					onFailure = { Load.Failed(it.userMessage()) },
				)
			_isRefreshing.value = false
		}
	}

	val servers: StateFlow<List<ServerConfig>> = selection.available
		.stateIn(viewModelScope, SharingStarted.Lazily, emptyList())

	val currentServer: StateFlow<ServerConfig?> = selection.current
		.stateIn(viewModelScope, SharingStarted.Lazily, null)

	fun selectServer(id: ServerId) = viewModelScope.launch { selection.select(id) }
}
