package org.gaindrive.android.ui.search

import androidx.lifecycle.ViewModel
import androidx.lifecycle.viewModelScope
import dagger.hilt.android.lifecycle.HiltViewModel
import kotlinx.coroutines.ExperimentalCoroutinesApi
import kotlinx.coroutines.FlowPreview
import kotlinx.coroutines.flow.Flow
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.SharingStarted
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.asStateFlow
import kotlinx.coroutines.flow.catch
import kotlinx.coroutines.flow.combine
import kotlinx.coroutines.flow.debounce
import kotlinx.coroutines.flow.flatMapLatest
import kotlinx.coroutines.flow.flow
import kotlinx.coroutines.flow.flowOf
import kotlinx.coroutines.flow.onStart
import kotlinx.coroutines.flow.stateIn
import kotlinx.coroutines.flow.update
import kotlinx.coroutines.launch
import org.gaindrive.android.data.LibraryRepository
import org.gaindrive.android.data.ServerFailure
import org.gaindrive.android.data.ServerSelection
import org.gaindrive.android.data.model.Artist
import org.gaindrive.android.data.model.BrowseScope
import org.gaindrive.android.data.model.ServerConfig
import org.gaindrive.android.data.model.ServerId
import org.gaindrive.android.net.userMessage
import org.gaindrive.android.ui.AlbumUi
import org.gaindrive.android.ui.SongUi
import javax.inject.Inject

/** Which categories to search. All three on by default, as in the web client. */
data class SearchFilters(
	val artists: Boolean = true,
	val albums: Boolean = true,
	val songs: Boolean = true,
) {
	val noneSelected: Boolean get() = !artists && !albums && !songs
}

data class SearchResults(
	val artists: List<Artist> = emptyList(),
	val albums: List<AlbumUi> = emptyList(),
	val songs: List<SongUi> = emptyList(),
) {
	val isEmpty: Boolean get() = artists.isEmpty() && albums.isEmpty() && songs.isEmpty()
}

/**
 * Separate from the shared [org.gaindrive.android.ui.Load] because search has a
 * fourth state the browse screens do not: nothing typed yet. Folding that into
 * "loaded but empty" would show "no results" before the user has asked
 * anything.
 */
sealed interface SearchPhase {
	data object Idle : SearchPhase
	data object Searching : SearchPhase
	data class Failed(val message: String) : SearchPhase

	/**
	 * [outstanding] is true while some servers have answered and others have
	 * not — the results are usable but not yet complete, which the screen shows
	 * as a quiet indicator rather than by withholding what it has.
	 */
	data class Ready(
		val results: SearchResults,
		val failures: List<ServerFailure> = emptyList(),
		val outstanding: Boolean = false,
	) : SearchPhase
}

@OptIn(ExperimentalCoroutinesApi::class, FlowPreview::class)
@HiltViewModel
class SearchViewModel @Inject constructor(
	private val library: LibraryRepository,
	private val selection: ServerSelection,
) : ViewModel() {

	private val _query = MutableStateFlow("")
	val query: StateFlow<String> = _query.asStateFlow()

	private val _filters = MutableStateFlow(SearchFilters())
	val filters: StateFlow<SearchFilters> = _filters.asStateFlow()

	/** Bumped to re-run the same query, e.g. from a pull. */
	private val reruns = MutableStateFlow(0)

	/** True only for a user-initiated pull, which drives the pull indicator. */
	private val _isRefreshing = MutableStateFlow(false)
	val isRefreshing: StateFlow<Boolean> = _isRefreshing.asStateFlow()

	val phase: StateFlow<SearchPhase> = combine(
		// Only the query is debounced. Toggling a filter is a deliberate act
		// and should re-run immediately, not after a pause.
		_query.debounce(DEBOUNCE_MS),
		_filters,
		// browse rather than scope: going offline has to re-run the search, or
		// the results on screen are still the server's.
		selection.browse,
		selection.scoped,
		reruns,
	) { query, filters, browse, servers, _ ->
		Search(query.trim(), filters, browse.scope, servers.size)
	}
		.flatMapLatest { search ->
			if (search.query.length < MIN_QUERY || search.filters.noneSelected) {
				_isRefreshing.value = false
				return@flatMapLatest flowOf(SearchPhase.Idle)
			}
			results(search)
		}
		// Lazily, not WhileSubscribed: leaving the tab dropped the last
		// subscriber, and returning restarted the upstream — silently re-running
		// the query and rebuilding results the user already had.
		.stateIn(viewModelScope, SharingStarted.Lazily, SearchPhase.Idle)

	/**
	 * One emission per server that answers, each carrying everything received
	 * so far. The fastest server's hits are on screen while the slowest is
	 * still thinking, which with several servers configured is usually the
	 * answer the user wanted.
	 */
	// flow<SearchPhase>, not flow: the builder is typed from its own emits
	// before the declared return type reaches it, so it would infer
	// Flow<SearchPhase.Ready> and refuse onStart's Searching.
	private fun results(search: Search): Flow<SearchPhase> = flow<SearchPhase> {
		val covers = library.coverUrls()
		var answered = 0

		library.searchProgressively(
			scope = search.scope,
			query = search.query,
			// Zero counts mean the server does no work for a category the
			// user has switched off.
			artistCount = if (search.filters.artists) ARTIST_LIMIT else 0,
			albumCount = if (search.filters.albums) ALBUM_LIMIT else 0,
			songCount = if (search.filters.songs) SONG_LIMIT else 0,
		).collect { merged ->
			answered++
			_isRefreshing.value = false
			emit(
				SearchPhase.Ready(
					results = SearchResults(
						artists = merged.items.artists,
						albums = merged.items.albums.map {
							AlbumUi(it, covers.url(it.coverArt, COVER_PX))
						},
						songs = merged.items.songs.map {
							SongUi(it, covers.url(it.coverArt, COVER_PX))
						},
					),
					failures = merged.failures,
					outstanding = answered < search.serverCount,
				)
			)
		}
	}
		// A pull keeps the results on screen; only a new query blanks them.
		.onStart { if (!_isRefreshing.value) emit(SearchPhase.Searching) }
		// The fan-out reports per-server problems as failures; anything that
		// escapes it failed for all of them.
		.catch { emit(SearchPhase.Failed(it.userMessage())) }

	private data class Search(
		val query: String,
		val filters: SearchFilters,
		val scope: BrowseScope,
		val serverCount: Int,
	)

	fun onQueryChange(value: String) {
		_query.value = value
	}

	fun clearQuery() {
		_query.value = ""
	}

	fun refresh() {
		_isRefreshing.value = true
		reruns.update { it + 1 }
	}

	fun toggleArtists() = _filters.update { it.copy(artists = !it.artists) }
	fun toggleAlbums() = _filters.update { it.copy(albums = !it.albums) }
	fun toggleSongs() = _filters.update { it.copy(songs = !it.songs) }

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

	private companion object {
		const val DEBOUNCE_MS = 300L

		/** One character matches most of a library; not worth the round trip. */
		const val MIN_QUERY = 2

		const val ARTIST_LIMIT = 20
		const val ALBUM_LIMIT = 30
		const val SONG_LIMIT = 60
		const val COVER_PX = 144
	}
}
