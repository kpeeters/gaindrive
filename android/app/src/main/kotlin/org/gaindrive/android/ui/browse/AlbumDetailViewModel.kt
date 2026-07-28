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
import org.gaindrive.android.data.LibraryRepository
import org.gaindrive.android.data.model.AlbumDetail
import org.gaindrive.android.data.model.ItemRef
import org.gaindrive.android.net.runCatchingCancellable
import org.gaindrive.android.net.userMessage
import org.gaindrive.android.ui.Load
import org.gaindrive.android.ui.Route
import javax.inject.Inject

data class AlbumDetailUi(
	val detail: AlbumDetail,
	val heroUrl: String?,
)

@HiltViewModel
class AlbumDetailViewModel @Inject constructor(
	private val library: LibraryRepository,
	savedStateHandle: SavedStateHandle,
) : ViewModel() {

	private val route = savedStateHandle.toRoute<Route.Album>()
	val albumRef: ItemRef =
		ItemRef.decode(route.albumRef) ?: error("Bad album ref: ${route.albumRef}")

	/** Shown in the app bar while the body is still loading. */
	val albumTitle: String = route.albumTitle

	private val _state = MutableStateFlow<Load<AlbumDetailUi>>(Load.Loading)
	val state: StateFlow<Load<AlbumDetailUi>> = _state.asStateFlow()

	init {
		load()
	}

	fun load() {
		_state.value = Load.Loading
		viewModelScope.launch {
			_state.value = runCatchingCancellable {
				val covers = library.coverUrls()
				val detail = library.albumDetail(albumRef)
					?: return@runCatchingCancellable null
				AlbumDetailUi(detail, covers.url(detail.album.coverArt, HERO_PX))
			}.fold(
				onSuccess = { ui ->
					if (ui == null) Load.Failed("That album is no longer on the server.")
					else Load.Ready(ui)
				},
				onFailure = { Load.Failed(it.userMessage()) },
			)
		}
	}

	private companion object {
		/** Big enough for a full-width hero on a dense screen. */
		const val HERO_PX = 800
	}
}
