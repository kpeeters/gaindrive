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
import org.gaindrive.android.data.ServerFailure
import org.gaindrive.android.data.ServerSection
import org.gaindrive.android.data.ServerSelection
import org.gaindrive.android.data.model.BrowseScope
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

	/**
	 * Sections rather than one list: a playlist belongs to one server, so in
	 * merged scope they sit under per-server headings instead of interleaved.
	 * With one server in scope there is a single section and the screen draws
	 * no heading at all.
	 */
	private val _state = MutableStateFlow<Load<List<ServerSection<Playlist>>>>(Load.Loading)
	val state: StateFlow<Load<List<ServerSection<Playlist>>>> = _state.asStateFlow()

	private val _failures = MutableStateFlow<List<ServerFailure>>(emptyList())
	val failures: StateFlow<List<ServerFailure>> = _failures.asStateFlow()

	/** True only for a user-initiated pull, which drives the pull indicator. */
	private val _isRefreshing = MutableStateFlow(false)
	val isRefreshing: StateFlow<Boolean> = _isRefreshing.asStateFlow()

	/** A failed edit, shown once in a snackbar. */
	private val _error = MutableStateFlow<String?>(null)
	val error: StateFlow<String?> = _error.asStateFlow()

	private var scope: BrowseScope = BrowseScope.AllServers

	/** Named apart from the exposed `offline` flow, which is the same value. */
	private var wasOffline: Boolean = false
	private var loadJob: Job? = null

	init {
		viewModelScope.launch {
			combine(
				selection.browse.distinctUntilChanged(),
				// Re-reads after a create or delete anywhere in the app.
				library.playlistRevision,
			) { selected, _ -> selected }.collect { selected ->
				// A different scope is a different set of playlists, so the old
				// list must go. A revision bump is the same list changed, and
				// blanking it there would flash the whole screen on every edit.
				// Going offline counts as switching: the source changed even
				// though the scope did not.
				val switched = selected.scope != scope || selected.offline != wasOffline
				scope = selected.scope
				wasOffline = selected.offline
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

	fun dismissFailures() {
		_failures.value = emptyList()
	}

	private fun startLoad(clearFirst: Boolean) {
		loadJob?.cancel()
		loadJob = viewModelScope.launch {
			if (clearFirst) _state.value = Load.Loading
			if (selection.hasNoServers()) {
				_state.value = Load.Failed("No server configured. Add one in Settings.")
				_isRefreshing.value = false
				return@launch
			}

			runCatchingCancellable { library.playlists(scope) }.fold(
				onSuccess = { merged ->
					if (merged.items.isEmpty() && merged.isPartial) {
						_state.value = Load.Failed(merged.failures.first().message)
					} else {
						_state.value = Load.Ready(merged.items)
					}
					_failures.value = merged.failures
				},
				onFailure = {
					_state.value = Load.Failed(it.userMessage())
					_failures.value = emptyList()
				},
			)
			_isRefreshing.value = false
		}
	}

	val servers: StateFlow<List<ServerConfig>> = selection.available
		.stateIn(viewModelScope, SharingStarted.Lazily, emptyList())

	val browseScope: StateFlow<BrowseScope> = selection.scope
		.stateIn(viewModelScope, SharingStarted.Lazily, BrowseScope.AllServers)

	/** Non-empty only when the sections need naming. */
	val badgeNames: StateFlow<Map<ServerId, String>> = selection.badgeNames
		.stateIn(viewModelScope, SharingStarted.Lazily, emptyMap())

	fun selectServer(id: ServerId) = viewModelScope.launch { selection.select(id) }

	fun selectAllServers() = viewModelScope.launch { selection.selectAllServers() }

	val offline: StateFlow<Boolean> = selection.offline
		.stateIn(viewModelScope, SharingStarted.Lazily, false)

	fun setOffline(enabled: Boolean) = viewModelScope.launch { selection.setOffline(enabled) }
}
