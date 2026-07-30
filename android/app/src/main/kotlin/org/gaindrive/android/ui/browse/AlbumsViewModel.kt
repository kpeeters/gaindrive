package org.gaindrive.android.ui.browse

import androidx.lifecycle.SavedStateHandle
import androidx.lifecycle.ViewModel
import androidx.lifecycle.viewModelScope
import androidx.navigation.toRoute
import dagger.hilt.android.lifecycle.HiltViewModel
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.asStateFlow
import kotlinx.coroutines.flow.update
import kotlinx.coroutines.launch
import org.gaindrive.android.data.CoverUrls
import org.gaindrive.android.data.LibraryRepository
import org.gaindrive.android.data.ServerFailure
import org.gaindrive.android.data.model.ArtistInfo
import org.gaindrive.android.data.model.ItemRef
import org.gaindrive.android.net.runCatchingCancellable
import org.gaindrive.android.net.userMessage
import org.gaindrive.android.ui.AlbumUi
import org.gaindrive.android.ui.Load
import org.gaindrive.android.ui.Route
import javax.inject.Inject

/**
 * The artist header. Held separately from the album list because the server
 * may reach out to MusicBrainz and Wikipedia to build it, which can take
 * seconds or fail — neither of which may delay the albums.
 */
data class ArtistHeaderUi(
	val portraitUrl: String? = null,
	val info: ArtistInfo? = null,
)

@HiltViewModel
class AlbumsViewModel @Inject constructor(
	private val library: LibraryRepository,
	savedStateHandle: SavedStateHandle,
) : ViewModel() {

	private val route = savedStateHandle.toRoute<Route.Albums>()

	/**
	 * Every server's id for this artist. More than one when the row that was
	 * tapped merged artists of the same name.
	 */
	val artistRefs: List<ItemRef> = ItemRef.decodeAll(route.artistRefs)
		.ifEmpty { error("Bad artist refs: ${route.artistRefs}") }

	/**
	 * The first contributor in registry order. Its server answers for the
	 * portrait and biography — those are one server's opinion of the artist,
	 * and showing two of them stacked would be worse than picking one.
	 */
	private val primaryRef: ItemRef = artistRefs.first()

	val artistName: String = route.artistName

	private val _state = MutableStateFlow<Load<List<AlbumUi>>>(Load.Loading)
	val state: StateFlow<Load<List<AlbumUi>>> = _state.asStateFlow()

	private val _header = MutableStateFlow(ArtistHeaderUi())
	val header: StateFlow<ArtistHeaderUi> = _header.asStateFlow()

	private val _failures = MutableStateFlow<List<ServerFailure>>(emptyList())
	val failures: StateFlow<List<ServerFailure>> = _failures.asStateFlow()

	/** True only for a user-initiated pull, which drives the pull indicator. */
	private val _isRefreshing = MutableStateFlow(false)
	val isRefreshing: StateFlow<Boolean> = _isRefreshing.asStateFlow()

	init {
		load()
	}

	/** Initial load and retry: there is nothing worth keeping on screen. */
	fun load() {
		_state.value = Load.Loading
		loadAlbums()
		loadHeader()
	}

	/** Pull to refresh: keep the albums visible while they are re-read. */
	fun refresh() {
		_isRefreshing.value = true
		loadAlbums()
		loadHeader()
	}

	fun dismissFailures() {
		_failures.value = emptyList()
	}

	private fun loadAlbums() = viewModelScope.launch {
		runCatchingCancellable {
			// Resolved once for the whole list, not per row.
			val covers: CoverUrls = library.coverUrls()
			// Badges here follow the artist, not the browse scope: this row
			// list is a union across the servers the merged artist came from,
			// and a single-server artist needs no badge on every row.
			val names =
				if (artistRefs.size < 2) emptyMap()
				else library.enabledServers().associate { it.id to it.name }

			library.albumsOfArtist(artistRefs).map { albums ->
				albums.map { album ->
					AlbumUi(
						album = album,
						coverUrl = covers.url(album.coverArt, COVER_PX),
						badges = album.sources.mapNotNull { names[it] },
					)
				}
			}
		}.fold(
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

	/**
	 * Fills in the header as it arrives. Portrait first, since that is a plain
	 * file the server already has; the biography may involve a lookup.
	 */
	private fun loadHeader() = viewModelScope.launch {
		runCatchingCancellable {
			val covers = library.coverUrls()
			covers.url(primaryRef, PORTRAIT_PX)
		}.getOrNull()?.let { url ->
			_header.update { it.copy(portraitUrl = url) }
		}

		// A missing biography is entirely normal and never worth an error.
		runCatchingCancellable { library.artistInfo(primaryRef) }
			.getOrNull()
			?.let { info -> _header.update { it.copy(info = info) } }
	}

	private companion object {
		/** Matches the 48dp thumbnail at roughly 3x density. */
		const val COVER_PX = 144

		/** Matches the 96dp avatar at roughly 3x density. */
		const val PORTRAIT_PX = 288
	}
}
