package org.gaindrive.android.ui.playlists

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
import org.gaindrive.android.data.model.ItemRef
import org.gaindrive.android.net.runCatchingCancellable
import org.gaindrive.android.net.userMessage
import org.gaindrive.android.ui.Load
import org.gaindrive.android.ui.Route
import org.gaindrive.android.ui.SongUi
import org.gaindrive.android.ui.valueOrNull
import javax.inject.Inject

@HiltViewModel
class PlaylistDetailViewModel @Inject constructor(
	private val library: LibraryRepository,
	savedStateHandle: SavedStateHandle,
) : ViewModel() {

	private val route = savedStateHandle.toRoute<Route.Playlist>()
	val playlistRef: ItemRef =
		ItemRef.decode(route.playlistRef) ?: error("Bad playlist ref: ${route.playlistRef}")

	/** Shown in the app bar while the body is still loading. */
	val playlistName: String = route.playlistName

	private val _state = MutableStateFlow<Load<List<SongUi>>>(Load.Loading)
	val state: StateFlow<Load<List<SongUi>>> = _state.asStateFlow()

	/** True only for a user-initiated pull, which drives the pull indicator. */
	private val _isRefreshing = MutableStateFlow(false)
	val isRefreshing: StateFlow<Boolean> = _isRefreshing.asStateFlow()

	/**
	 * A removal in flight. Removal is by *position*, so a second one issued
	 * before the first lands would carry an index the server has already
	 * shifted and delete the wrong track.
	 */
	private val _removing = MutableStateFlow(false)
	val removing: StateFlow<Boolean> = _removing.asStateFlow()

	private val _error = MutableStateFlow<String?>(null)
	val error: StateFlow<String?> = _error.asStateFlow()

	init {
		load()
	}

	fun load() {
		_state.value = Load.Loading
		fetch()
	}

	fun refresh() {
		_isRefreshing.value = true
		fetch()
	}

	fun remove(index: Int) {
		if (_removing.value) return
		val songs = _state.value.valueOrNull() ?: return
		if (index !in songs.indices) return

		_removing.value = true
		viewModelScope.launch {
			runCatchingCancellable { library.removeFromPlaylist(playlistRef, index) }
				.fold(
					// Dropped locally rather than by re-reading the playlist:
					// the server has confirmed this exact position, and a
					// refetch would make every removal cost a round trip the
					// user watches.
					onSuccess = {
						_state.value = Load.Ready(songs.filterIndexed { at, _ -> at != index })
					},
					onFailure = { _error.value = "Could not remove: ${it.userMessage()}" },
				)
			_removing.value = false
		}
	}

	fun clearError() {
		_error.value = null
	}

	private fun fetch() {
		viewModelScope.launch {
			// Failing to resolve cover URLs costs thumbnails, not the track
			// list, so it is caught separately rather than failing the screen.
			val covers = runCatchingCancellable { library.coverUrls() }.getOrNull()

			_state.value = runCatchingCancellable { library.playlist(playlistRef) }.fold(
				onSuccess = { playlist ->
					if (playlist == null) {
						Load.Failed("That playlist is no longer on the server.")
					} else {
						Load.Ready(
							playlist.songs.map { SongUi(it, covers?.url(it.coverArt, COVER_PX)) }
						)
					}
				},
				onFailure = { Load.Failed(it.userMessage()) },
			)
			_isRefreshing.value = false
		}
	}

	private companion object {
		/** Matches the 40dp thumbnail at roughly 3x density. */
		const val COVER_PX = 144
	}
}
