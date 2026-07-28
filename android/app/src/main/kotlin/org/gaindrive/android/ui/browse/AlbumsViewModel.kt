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
import org.gaindrive.android.data.model.Album
import org.gaindrive.android.data.model.ArtistInfo
import org.gaindrive.android.data.model.ItemRef
import org.gaindrive.android.net.runCatchingCancellable
import org.gaindrive.android.net.userMessage
import org.gaindrive.android.ui.Load
import org.gaindrive.android.ui.Route
import javax.inject.Inject

/** An album plus the cover URL already resolved, so the row stays dumb. */
data class AlbumUi(val album: Album, val coverUrl: String?)

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
	val artistRef: ItemRef =
		ItemRef.decode(route.artistRef) ?: error("Bad artist ref: ${route.artistRef}")
	val artistName: String = route.artistName

	private val _state = MutableStateFlow<Load<List<AlbumUi>>>(Load.Loading)
	val state: StateFlow<Load<List<AlbumUi>>> = _state.asStateFlow()

	private val _header = MutableStateFlow(ArtistHeaderUi())
	val header: StateFlow<ArtistHeaderUi> = _header.asStateFlow()

	init {
		load()
	}

	fun load() {
		_state.value = Load.Loading
		loadAlbums()
		loadHeader()
	}

	private fun loadAlbums() = viewModelScope.launch {
		_state.value = runCatchingCancellable {
			// Resolved once for the whole list, not per row.
			val covers: CoverUrls = library.coverUrls()
			library.albumsOfArtist(artistRef)
				.map { AlbumUi(it, covers.url(it.coverArt, COVER_PX)) }
		}.fold(
			onSuccess = { Load.Ready(it) },
			onFailure = { Load.Failed(it.userMessage()) },
		)
	}

	/**
	 * Fills in the header as it arrives. Portrait first, since that is a plain
	 * file the server already has; the biography may involve a lookup.
	 */
	private fun loadHeader() = viewModelScope.launch {
		runCatchingCancellable {
			val covers = library.coverUrls()
			covers.url(artistRef, PORTRAIT_PX)
		}.getOrNull()?.let { url ->
			_header.update { it.copy(portraitUrl = url) }
		}

		// A missing biography is entirely normal and never worth an error.
		runCatchingCancellable { library.artistInfo(artistRef) }
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
