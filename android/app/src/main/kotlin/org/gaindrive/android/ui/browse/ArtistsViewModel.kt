package org.gaindrive.android.ui.browse

import androidx.lifecycle.SavedStateHandle
import androidx.lifecycle.ViewModel
import androidx.lifecycle.viewModelScope
import androidx.navigation.toRoute
import dagger.hilt.android.lifecycle.HiltViewModel
import kotlinx.coroutines.Job
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.SharingStarted
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.asStateFlow
import kotlinx.coroutines.flow.distinctUntilChanged
import kotlinx.coroutines.flow.stateIn
import kotlinx.coroutines.launch
import org.gaindrive.android.data.FetchMonitor
import org.gaindrive.android.data.FetchStatus
import org.gaindrive.android.data.LibraryRepository
import org.gaindrive.android.data.ServerFailure
import org.gaindrive.android.data.ServerSelection
import org.gaindrive.android.data.model.BrowseScope
import org.gaindrive.android.data.model.LibraryListing
import org.gaindrive.android.data.model.ServerConfig
import org.gaindrive.android.data.model.ServerId
import org.gaindrive.android.net.runCatchingCancellable
import org.gaindrive.android.net.userMessage
import org.gaindrive.android.ui.Load
import org.gaindrive.android.ui.Route
import javax.inject.Inject

@HiltViewModel
class ArtistsViewModel @Inject constructor(
	private val library: LibraryRepository,
	private val selection: ServerSelection,
	monitor: FetchMonitor,
	savedStateHandle: SavedStateHandle,
) : ViewModel() {

	/**
	 * Whether this instance is the uploads listing rather than the library.
	 * From the route, so the pushed `Route.Uploads` host and the Library tab
	 * share every line of this class — the listing they load is the whole
	 * difference.
	 */
	val uploads: Boolean = savedStateHandle.toRoute<Route.Artists>().uploads

	/**
	 * What is being fetched, for the uploads listing's own row.
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
	private val _state = MutableStateFlow<Load<LibraryListing>>(Load.Loading)
	val state: StateFlow<Load<LibraryListing>> = _state.asStateFlow()

	/** Servers that did not answer this load, if any did. */
	private val _failures = MutableStateFlow<List<ServerFailure>>(emptyList())
	val failures: StateFlow<List<ServerFailure>> = _failures.asStateFlow()

	/** True only for a user-initiated pull, which drives the pull indicator. */
	private val _isRefreshing = MutableStateFlow(false)
	val isRefreshing: StateFlow<Boolean> = _isRefreshing.asStateFlow()

	/**
	 * Whether the upload icon is worth drawing. Never true on the uploads
	 * listing itself — the icon is how you get there.
	 */
	private val _canUpload = MutableStateFlow(false)
	val canUpload: StateFlow<Boolean> = _canUpload.asStateFlow()

	private var scope: BrowseScope = BrowseScope.AllServers
	private var loadJob: Job? = null

	init {
		viewModelScope.launch {
			// distinctUntilChanged because the registry re-emits whenever
			// anything in DataStore changes, and an unchanged scope is not a
			// reason to re-read the library.
			selection.browse.distinctUntilChanged().collect { selected ->
				scope = selected.scope
				refreshCanUpload()
				// A different scope is a different library, and going offline
				// is the same library from a different source — either way the
				// old list must go rather than linger under a spinner.
				startLoad(clearFirst = true)
			}
		}
	}

	/**
	 * Failure keeps the last answer rather than hiding the icon: it is
	 * navigation, and taking it away because one request timed out would strand
	 * the user out of their own uploads.
	 */
	private fun refreshCanUpload() {
		if (uploads) return
		viewModelScope.launch {
			runCatchingCancellable { library.canUpload(scope) }
				.getOrNull()?.let { _canUpload.value = it }
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
			if (clearFirst) _state.value = Load.Loading
			if (selection.hasNoServers()) {
				_state.value = Load.Failed("No server configured. Add one in Settings.")
				_isRefreshing.value = false
				return@launch
			}

			val loaded = runCatchingCancellable {
				if (uploads) {
					// The personal slice has no categories by construction, so
					// it is an artists-only listing of the same shape.
					library.uploadIndexes(scope).map { LibraryListing(emptyList(), it) }
				} else {
					library.libraryListing(scope)
				}
			}
			loaded.fold(
				onSuccess = { merged ->
					// Every server failing is a failed screen; some of them
					// failing is a note over the ones that worked.
					if (merged.items.isEmpty && merged.isPartial) {
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
