package org.gaindrive.android.ui.browse

import androidx.lifecycle.ViewModel
import androidx.lifecycle.viewModelScope
import dagger.hilt.android.lifecycle.HiltViewModel
import kotlinx.coroutines.Job
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.SharingStarted
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.asStateFlow
import kotlinx.coroutines.flow.distinctUntilChanged
import kotlinx.coroutines.flow.stateIn
import kotlinx.coroutines.launch
import org.gaindrive.android.data.LibraryRepository
import org.gaindrive.android.data.ServerSelection
import org.gaindrive.android.data.model.ArtistIndex
import org.gaindrive.android.data.model.ServerConfig
import org.gaindrive.android.data.model.ServerId
import org.gaindrive.android.net.runCatchingCancellable
import org.gaindrive.android.net.userMessage
import org.gaindrive.android.ui.Load
import javax.inject.Inject

@HiltViewModel
class ArtistsViewModel @Inject constructor(
	private val library: LibraryRepository,
	private val selection: ServerSelection,
) : ViewModel() {

	/**
	 * A plain held value, not a `stateIn(WhileSubscribed(…))` over the query.
	 *
	 * That was the earlier shape and it re-fetched the whole artist list every
	 * time the screen came back into view: leaving for an album dropped the last
	 * subscriber, the share stopped, and returning restarted the upstream. The
	 * list does not change while the user is two screens deep, so it is simply
	 * kept until something asks for it again.
	 */
	private val _state = MutableStateFlow<Load<List<ArtistIndex>>>(Load.Loading)
	val state: StateFlow<Load<List<ArtistIndex>>> = _state.asStateFlow()

	/** True only for a user-initiated pull, which drives the pull indicator. */
	private val _isRefreshing = MutableStateFlow(false)
	val isRefreshing: StateFlow<Boolean> = _isRefreshing.asStateFlow()

	private var server: ServerConfig? = null
	private var loadJob: Job? = null

	init {
		viewModelScope.launch {
			// distinctUntilChanged because the registry re-emits whenever
			// anything in DataStore changes, and an unchanged server is not a
			// reason to re-read the library.
			selection.current.distinctUntilChanged().collect { selected ->
				server = selected
				// A different server is a different library, so the old list
				// must go rather than linger under a spinner.
				startLoad(clearFirst = true)
			}
		}
	}

	/** Retry after a failure: there is nothing worth keeping on screen. */
	fun load() = startLoad(clearFirst = true)

	/** Pull to refresh: keep the list visible while it is re-read. */
	fun refresh() {
		_isRefreshing.value = true
		startLoad(clearFirst = false)
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

			_state.value = runCatchingCancellable { library.artistIndexes(current.id) }
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
