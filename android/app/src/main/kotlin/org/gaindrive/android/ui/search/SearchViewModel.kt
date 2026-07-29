package org.gaindrive.android.ui.search

import androidx.lifecycle.ViewModel
import androidx.lifecycle.viewModelScope
import dagger.hilt.android.lifecycle.HiltViewModel
import kotlinx.coroutines.ExperimentalCoroutinesApi
import kotlinx.coroutines.FlowPreview
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.SharingStarted
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.asStateFlow
import kotlinx.coroutines.flow.combine
import kotlinx.coroutines.flow.debounce
import kotlinx.coroutines.flow.flatMapLatest
import kotlinx.coroutines.flow.flow
import kotlinx.coroutines.flow.stateIn
import kotlinx.coroutines.flow.update
import org.gaindrive.android.data.LibraryRepository
import org.gaindrive.android.data.ServerSelection
import org.gaindrive.android.data.model.Artist
import org.gaindrive.android.net.runCatchingCancellable
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
	data class Ready(val results: SearchResults) : SearchPhase
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

	val phase: StateFlow<SearchPhase> = combine(
		// Only the query is debounced. Toggling a filter is a deliberate act
		// and should re-run immediately, not after a pause.
		_query.debounce(DEBOUNCE_MS),
		_filters,
		selection.current,
	) { query, filters, server -> Triple(query.trim(), filters, server) }
		.flatMapLatest { (query, filters, server) ->
			flow {
				if (query.length < MIN_QUERY || server == null || filters.noneSelected) {
					emit(SearchPhase.Idle)
					return@flow
				}
				emit(SearchPhase.Searching)
				emit(
					runCatchingCancellable {
						val covers = library.coverUrls()
						val found = library.search(
							server = server.id,
							query = query,
							// Zero counts mean the server does no work for a
							// category the user has switched off.
							artistCount = if (filters.artists) ARTIST_LIMIT else 0,
							albumCount = if (filters.albums) ALBUM_LIMIT else 0,
							songCount = if (filters.songs) SONG_LIMIT else 0,
						)
						SearchResults(
							artists = found.artists,
							albums = found.albums.map {
								AlbumUi(it, covers.url(it.coverArt, COVER_PX))
							},
							songs = found.songs.map {
								SongUi(it, covers.url(it.coverArt, COVER_PX))
							},
						)
					}.fold(
						onSuccess = { SearchPhase.Ready(it) },
						onFailure = { SearchPhase.Failed(it.userMessage()) },
					)
				)
			}
		}
		.stateIn(viewModelScope, SharingStarted.WhileSubscribed(5_000), SearchPhase.Idle)

	fun onQueryChange(value: String) {
		_query.value = value
	}

	fun clearQuery() {
		_query.value = ""
	}

	fun toggleArtists() = _filters.update { it.copy(artists = !it.artists) }
	fun toggleAlbums() = _filters.update { it.copy(albums = !it.albums) }
	fun toggleSongs() = _filters.update { it.copy(songs = !it.songs) }

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
