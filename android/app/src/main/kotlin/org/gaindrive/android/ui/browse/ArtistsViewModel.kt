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
import kotlinx.coroutines.flow.first
import kotlinx.coroutines.flow.stateIn
import kotlinx.coroutines.launch
import org.gaindrive.android.data.FetchMonitor
import org.gaindrive.android.data.FetchStatus
import org.gaindrive.android.data.LibraryRepository
import org.gaindrive.android.data.ServerFailure
import org.gaindrive.android.data.ServerSelection
import org.gaindrive.android.data.SettingsStore
import org.gaindrive.android.data.model.ArtistIndex
import org.gaindrive.android.data.model.BrowseScope
import org.gaindrive.android.data.model.LibraryMode
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
	private val settings: SettingsStore,
	monitor: FetchMonitor,
) : ViewModel() {

	/**
	 * What is being fetched, for the Uploads slice's own row.
	 *
	 * Here as well as in the shell's strip because this is where somebody looks
	 * when they wonder whether a fetch is running — the report that prompted all
	 * of this was "came back to my uploads folder and saw nothing in progress".
	 * The strip answers it from every screen; this answers it at the place the
	 * question is actually asked.
	 *
	 * The listing itself needs nothing: a finished fetch bumps [LibraryRevision],
	 * which ServerSelection.browse already folds into the value collected below.
	 */
	val fetches: StateFlow<FetchStatus> =
		monitor.status.stateIn(viewModelScope, SharingStarted.WhileSubscribed(5_000), FetchStatus())

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

	/** Servers that did not answer this load, if any did. */
	private val _failures = MutableStateFlow<List<ServerFailure>>(emptyList())
	val failures: StateFlow<List<ServerFailure>> = _failures.asStateFlow()

	/** True only for a user-initiated pull, which drives the pull indicator. */
	private val _isRefreshing = MutableStateFlow(false)
	val isRefreshing: StateFlow<Boolean> = _isRefreshing.asStateFlow()

	/** The kinds on offer, and which one is showing. */
	private val _modes = MutableStateFlow(listOf(LibraryMode.ARTISTS))
	val modes: StateFlow<List<LibraryMode>> = _modes.asStateFlow()

	private val _mode = MutableStateFlow(LibraryMode.ARTISTS)
	val mode: StateFlow<LibraryMode> = _mode.asStateFlow()

	private var scope: BrowseScope = BrowseScope.AllServers
	private var loadJob: Job? = null

	init {
		viewModelScope.launch {
			// The stored mode is read once, before the first load, so the
			// first list drawn is already the right kind rather than artists
			// flashing past on the way to categories.
			settings.libraryMode.first()?.let { _mode.value = LibraryMode(it) }

			// distinctUntilChanged because the registry re-emits whenever
			// anything in DataStore changes, and an unchanged scope is not a
			// reason to re-read the library.
			selection.browse.distinctUntilChanged().collect { selected ->
				scope = selected.scope
				// Awaited rather than launched alongside the load, because it
				// may *change* the selected chip — a server offering only
				// folders has no "artists" among them — and loading first would
				// then query a chip nothing answers for and leave the correction
				// with no reload behind it. It costs nothing to wait: both this
				// and the load below read the same per-session root cache, so
				// only one of them reaches the network.
				refreshModes()
				// A different scope is a different library, and going offline
				// is the same library from a different source — either way the
				// old list must go rather than linger under a spinner.
				startLoad(clearFirst = true)
			}
		}
	}

	fun selectMode(next: LibraryMode) {
		if (next == _mode.value) return
		_mode.value = next
		viewModelScope.launch { settings.setLibraryMode(next.id) }
		startLoad(clearFirst = true)
	}

	/**
	 * Re-reads which slices this scope offers, and falls back when the stored
	 * one is gone — a server may have been removed since it was chosen, or
	 * switched to browsing by folder, which replaces its chips entirely.
	 * Failure leaves the current list alone: the chips are navigation, and
	 * losing them because one server timed out would be worse than showing a
	 * stale set.
	 *
	 * The caller must load *after* this, not alongside it; see the collector.
	 */
	private suspend fun refreshModes() {
		val available = runCatchingCancellable { library.availableModes(scope) }
			.getOrNull() ?: return
		_modes.value = available
		if (_mode.value !in available) _mode.value = available.first()
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
			if (clearFirst) _state.value = Load.Loading
			if (selection.hasNoServers()) {
				_state.value = Load.Failed("No server configured. Add one in Settings.")
				_isRefreshing.value = false
				return@launch
			}

			runCatchingCancellable { library.artistIndexes(scope, _mode.value) }.fold(
				onSuccess = { merged ->
					// Every server failing is a failed screen; some of them
					// failing is a note over the ones that worked.
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

	fun dismissFailures() {
		_failures.value = emptyList()
	}

	val servers: StateFlow<List<ServerConfig>> = selection.available
		.stateIn(viewModelScope, SharingStarted.Lazily, emptyList())

	val browseScope: StateFlow<BrowseScope> = selection.scope
		.stateIn(viewModelScope, SharingStarted.Lazily, BrowseScope.AllServers)

	val badgeNames: StateFlow<Map<ServerId, String>> = selection.badgeNames
		.stateIn(viewModelScope, SharingStarted.Lazily, emptyMap())

	fun selectServer(id: ServerId) = viewModelScope.launch { selection.select(id) }

	fun selectAllServers() = viewModelScope.launch { selection.selectAllServers() }

	val offline: StateFlow<Boolean> = selection.offline
		.stateIn(viewModelScope, SharingStarted.Lazily, false)

	fun setOffline(enabled: Boolean) = viewModelScope.launch { selection.setOffline(enabled) }
}
