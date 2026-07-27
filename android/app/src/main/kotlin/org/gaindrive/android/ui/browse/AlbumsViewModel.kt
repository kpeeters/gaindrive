package org.gaindrive.android.ui.browse

import androidx.lifecycle.SavedStateHandle
import androidx.lifecycle.ViewModel
import androidx.lifecycle.viewModelScope
import androidx.navigation.toRoute
import dagger.hilt.android.lifecycle.HiltViewModel
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.asStateFlow
import kotlinx.coroutines.launch
import org.gaindrive.android.data.CoverUrls
import org.gaindrive.android.data.LibraryRepository
import org.gaindrive.android.data.model.Album
import org.gaindrive.android.data.model.ItemRef
import org.gaindrive.android.ui.Load
import org.gaindrive.android.ui.Route
import javax.inject.Inject

/** An album plus the cover URL already resolved, so the row stays dumb. */
data class AlbumUi(val album: Album, val coverUrl: String?)

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

	init {
		load()
	}

	fun load() {
		_state.value = Load.Loading
		viewModelScope.launch {
			_state.value = runCatching {
				// Resolved once for the whole list, not per row.
				val covers: CoverUrls = library.coverUrls()
				library.albumsOfArtist(artistRef)
					.map { AlbumUi(it, covers.url(it.coverArt, COVER_PX)) }
			}.fold(
				onSuccess = { Load.Ready(it) },
				onFailure = { Load.Failed(it.message ?: "Could not reach the server") },
			)
		}
	}

	private companion object {
		/** Matches the 48dp thumbnail at roughly 3x density. */
		const val COVER_PX = 144
	}
}
