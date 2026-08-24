package org.gaindrive.android.ui.browse

import androidx.lifecycle.SavedStateHandle
import androidx.lifecycle.ViewModel
import androidx.lifecycle.viewModelScope
import androidx.navigation.toRoute
import dagger.hilt.android.lifecycle.HiltViewModel
import kotlinx.coroutines.async
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.asStateFlow
import kotlinx.coroutines.flow.update
import kotlinx.coroutines.launch
import org.gaindrive.android.data.LibraryRepository
import org.gaindrive.android.data.model.AlbumDetail
import org.gaindrive.android.data.model.AlbumNotes
import org.gaindrive.android.data.model.ItemRef
import org.gaindrive.android.net.runCatchingCancellable
import org.gaindrive.android.net.userMessage
import org.gaindrive.android.ui.Load
import org.gaindrive.android.ui.Route
import org.gaindrive.android.ui.valueOrNull
import javax.inject.Inject

/**
 * Everything on the screen that is not the track list. Held separately so the
 * tracks can go up the moment `getAlbum` answers: the cover URL needs a
 * DataStore read and a password decrypt, and the notes may send the server off
 * to MusicBrainz and Wikipedia. Neither may hold up the list.
 */
data class AlbumExtrasUi(
	val heroUrl: String? = null,
	val notes: AlbumNotes? = null,
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

	/**
	 * Whether moving this album into the shared library is on offer.
	 *
	 * Both halves are needed and neither is guessable from the ref: the route
	 * says the user reached this through Uploads, and the server says this
	 * account administers it. The server checks the second one again — this only
	 * decides whether to draw a control, never whether the move is allowed.
	 */
	private val _canPromote = MutableStateFlow(false)
	val canPromote: StateFlow<Boolean> = _canPromote.asStateFlow()

	/** Set once the move has succeeded, so the screen can leave. */
	private val _promoted = MutableStateFlow(false)
	val promoted: StateFlow<Boolean> = _promoted.asStateFlow()

	private val _promoting = MutableStateFlow(false)
	val promoting: StateFlow<Boolean> = _promoting.asStateFlow()

	private val _promoteError = MutableStateFlow<String?>(null)
	val promoteError: StateFlow<String?> = _promoteError.asStateFlow()

	private val _state = MutableStateFlow<Load<AlbumDetail>>(Load.Loading)
	val state: StateFlow<Load<AlbumDetail>> = _state.asStateFlow()

	private val _extras = MutableStateFlow(AlbumExtrasUi())
	val extras: StateFlow<AlbumExtrasUi> = _extras.asStateFlow()

	/** True only for a user-initiated pull, which drives the pull indicator. */
	private val _isRefreshing = MutableStateFlow(false)
	val isRefreshing: StateFlow<Boolean> = _isRefreshing.asStateFlow()

	init {
		load()
		// Only asked when the route says it could matter, so an ordinary album
		// costs no request at all.
		if (route.fromUploads) {
			viewModelScope.launch {
				_canPromote.value = runCatchingCancellable {
					library.isAdminOn(albumRef.server)
				}.getOrDefault(false)
			}
		}
	}

	/**
	 * Moves this album out of the uploads area into the shared library.
	 *
	 * The repository drops that server's mirror and bumps the library revision,
	 * so both listings are re-read; this only has to send the screen back, since
	 * the album it is showing is at a new id the moment this returns.
	 */
	fun promote() {
		if (_promoting.value) return
		_promoting.value = true
		_promoteError.value = null
		viewModelScope.launch {
			runCatchingCancellable { library.promoteAlbum(albumRef) }.fold(
				onSuccess = { _promoted.value = true },
				onFailure = { _promoteError.value = "Could not move it: ${it.userMessage()}" },
			)
			_promoting.value = false
		}
	}

	fun clearPromoteError() {
		_promoteError.value = null
	}

	/** Initial load and retry. */
	fun load() {
		_state.value = Load.Loading
		fetch()
	}

	/** Pull to refresh: keep the tracks visible while they are re-read. */
	fun refresh() {
		_isRefreshing.value = true
		fetch()
	}

	private fun fetch() {
		viewModelScope.launch {
			// Started alongside the track request rather than before it: it
			// touches DataStore and the keystore, and its result is only
			// needed once the tracks are already on screen.
			val covers = async { runCatchingCancellable { library.coverUrls() }.getOrNull() }

			val loaded: Load<AlbumDetail> =
				runCatchingCancellable { library.albumDetail(albumRef) }.fold(
					onSuccess = { detail ->
						if (detail == null) Load.Failed("That album is no longer on the server.")
						else Load.Ready(detail)
					},
					onFailure = { Load.Failed(it.userMessage()) },
				)
			_state.value = loaded
			_isRefreshing.value = false

			val detail = loaded.valueOrNull()
			if (detail == null) {
				covers.cancel()
				return@launch
			}

			// Tracks are visible from here on; each extra fills in as it
			// arrives and a failure costs only that one piece.
			covers.await()?.url(detail.album.coverArt, HERO_PX)
				?.let { url -> _extras.update { it.copy(heroUrl = url) } }

			// Absent album notes are entirely normal and never worth an error.
			runCatchingCancellable { library.albumNotes(albumRef) }
				.getOrNull()?.takeIf { !it.isEmpty }
				?.let { notes -> _extras.update { it.copy(notes = notes) } }
		}
	}

	private companion object {
		/** Big enough for a full-width hero on a dense screen. */
		const val HERO_PX = 800
	}
}
