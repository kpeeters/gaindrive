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
import org.gaindrive.android.data.model.Chapter
import org.gaindrive.android.data.model.ItemRef
import org.gaindrive.android.data.model.MusicRoot
import org.gaindrive.android.data.model.StarKind
import org.gaindrive.android.net.runCatchingCancellable
import org.gaindrive.android.net.userMessage
import org.gaindrive.android.ui.Load
import org.gaindrive.android.ui.Route
import org.gaindrive.android.ui.valueOrNull
import java.time.Instant
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
	/**
	 * The chapter markers of any chaptered recording in this folder, keyed by
	 * the recording. Empty for almost every album. Mirrored, so an album
	 * opened once online keeps its markers offline.
	 *
	 * An extra rather than part of the track list, on the same reasoning as the
	 * notes: it reads an endpoint older servers do not implement, and a failure
	 * must cost the chapter rows and not the tracks. It is requested alongside
	 * `getAlbum` rather than after it, since it normally answers first and the
	 * rows are then in place before anything is drawn.
	 */
	val chapters: Map<ItemRef, List<Chapter>> = emptyMap(),
)

/**
 * A track to start once the listing has loaded, and where in it.
 *
 * Only a chapter search hit produces one: a marker has no id anything can
 * stream, so acting on one means opening the recording's album and starting the
 * recording at that point. Consumed once - see [AlbumDetailViewModel.autoPlay].
 */
data class AutoPlay(val songIndex: Int, val positionMs: Long)

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
	 * account administers it. The server checks the second one again - this only
	 * decides whether to draw a control, never whether the move is allowed.
	 */
	private val _canPromote = MutableStateFlow(false)
	val canPromote: StateFlow<Boolean> = _canPromote.asStateFlow()

	/**
	 * Whether deleting this upload is on offer.
	 *
	 * The route flag alone, with no permission call behind it: **reaching an
	 * album through Uploads is ownership**, since every personal listing keys on
	 * the caller's own username, and deleting your own staging area is not an
	 * administrative act the way promoting into the shared library is. The
	 * server checks again regardless - this only decides whether to draw a
	 * control.
	 */
	val canDelete: Boolean = route.fromUploads

	/** Set once the move or the deletion has succeeded, so the screen can leave. */
	private val _promoted = MutableStateFlow(false)
	val promoted: StateFlow<Boolean> = _promoted.asStateFlow()

	private val _deleting = MutableStateFlow(false)
	val deleting: StateFlow<Boolean> = _deleting.asStateFlow()

	private val _promoting = MutableStateFlow(false)
	val promoting: StateFlow<Boolean> = _promoting.asStateFlow()

	private val _promoteError = MutableStateFlow<String?>(null)
	val promoteError: StateFlow<String?> = _promoteError.asStateFlow()

	/**
	 * Where it could go: the destination roots, and the level-1 folders of
	 * whichever is chosen.
	 *
	 * A root and one level is the whole of the destination - both layouts are
	 * `L1/L2/[L3]/files`, L2 is this album, and L3 is only ever a disc or season
	 * directory inside it - so this is two controls rather than a folder
	 * browser.
	 */
	private val _roots = MutableStateFlow<List<MusicRoot>>(emptyList())
	val roots: StateFlow<List<MusicRoot>> = _roots.asStateFlow()

	private val _destRoot = MutableStateFlow<MusicRoot?>(null)
	val destRoot: StateFlow<MusicRoot?> = _destRoot.asStateFlow()

	private val _folder = MutableStateFlow("")
	val folder: StateFlow<String> = _folder.asStateFlow()

	private val _folderSuggestions = MutableStateFlow<List<String>>(emptyList())
	val folderSuggestions: StateFlow<List<String>> = _folderSuggestions.asStateFlow()

	/** The name the batch is filed under now, which an artists root defaults to. */
	private val currentArtist: String?
		get() = _state.value.valueOrNull()?.album?.artistName?.takeIf { it.isNotBlank() }

	private val _state = MutableStateFlow<Load<AlbumDetail>>(Load.Loading)
	val state: StateFlow<Load<AlbumDetail>> = _state.asStateFlow()

	private val _extras = MutableStateFlow(AlbumExtrasUi())
	val extras: StateFlow<AlbumExtrasUi> = _extras.asStateFlow()

	/**
	 * Set once, after the first successful load, when the route asked for a
	 * track to be started.
	 *
	 * Published rather than acted on here: this class holds no reference to the
	 * player, and the screen already watches [promoted] the same way. The
	 * one-shot guard is what stops a pull to refresh, a rotation or a return to
	 * the screen restarting a recording the user has since moved on from.
	 */
	private val _autoPlay = MutableStateFlow<AutoPlay?>(null)
	val autoPlay: StateFlow<AutoPlay?> = _autoPlay.asStateFlow()

	private var autoPlayDone = false

	fun consumeAutoPlay() {
		_autoPlay.value = null
	}

	/** True only for a user-initiated pull, which drives the pull indicator. */
	private val _isRefreshing = MutableStateFlow(false)
	val isRefreshing: StateFlow<Boolean> = _isRefreshing.asStateFlow()

	/**
	 * A star tapped but not yet answered for.
	 *
	 * It disables the control rather than merely being drawn, because two taps
	 * in flight at once leave two requests racing to decide the final state and
	 * the loser wins about half the time.
	 */
	private val _starring = MutableStateFlow(false)
	val starring: StateFlow<Boolean> = _starring.asStateFlow()

	/** A star the server would not take, for a toast. */
	private val _starError = MutableStateFlow<String?>(null)
	val starError: StateFlow<String?> = _starError.asStateFlow()

	fun consumeStarError() {
		_starError.value = null
	}

	/**
	 * Stars or unstars the album, drawing the new state before the server has
	 * agreed to it.
	 *
	 * Optimistic because the round trip is long enough that a star waiting for
	 * it reads as a dead control, and reverted on failure because the one thing
	 * worse than a slow star is a star claiming the server knows something it
	 * never heard. Starring requires the network, so the stale `starredAt` this
	 * can leave in the offline mirror is replaced by the next load that reaches
	 * the server.
	 */
	fun toggleStar() {
		if (_starring.value) return
		val album = _state.value.valueOrNull()?.album ?: return
		val wanted = !album.isStarred
		_starring.value = true
		applyStar(wanted)
		viewModelScope.launch {
			runCatchingCancellable {
				library.setStarred(albumRef, StarKind.ALBUM, wanted)
			}.onFailure {
				applyStar(!wanted)
				_starError.value = it.userMessage()
			}
			_starring.value = false
		}
	}

	/**
	 * Written into [state] rather than held beside it, so a reload is simply
	 * authoritative and there is no second copy to reconcile with it.
	 *
	 * The timestamp is invented, which is safe only because nothing reads it:
	 * `Album.isStarred` asks whether it is there, never when it was, and the
	 * server's own answer overwrites it on the next load.
	 */
	private fun applyStar(starred: Boolean) {
		_state.update { current ->
			val detail = current.valueOrNull() ?: return@update current
			Load.Ready(
				detail.copy(
					album = detail.album.copy(
						starredAt = if (starred) Instant.now().toString() else null,
					)
				)
			)
		}
	}

	init {
		load()
		// Only asked when the route says it could matter, so an ordinary album
		// costs no request at all.
		if (route.fromUploads) {
			viewModelScope.launch {
				_canPromote.value = runCatchingCancellable {
					library.isAdminOn(albumRef.server)
				}.getOrDefault(false)
				// Only once the answer is yes: a non-admin never opens the
				// dialog, so fetching what it would contain is a request for
				// nothing.
				if (_canPromote.value) loadRoots()
			}
		}
	}

	private suspend fun loadRoots() {
		val found = runCatchingCancellable {
			library.destinationRoots(albumRef.server)
		}.getOrDefault(emptyList())
		_roots.value = found
		// Pre-selected rather than left empty, so the common single-root case
		// needs no interaction at all and the dialog is just a confirmation.
		found.firstOrNull()?.let { selectRoot(it) }
	}

	fun selectRoot(root: MusicRoot) {
		_destRoot.value = root
		// An artists root defaults to what the album is already filed under. A
		// categories root deliberately does not: L1 there is a *category*, and
		// the batch's artist is whatever the source called it - for a fetched
		// video, the channel name, which is never the answer.
		_folder.value = if (root.contentType == "categories") "" else currentArtist.orEmpty()
		_folderSuggestions.value = emptyList()
		viewModelScope.launch {
			_folderSuggestions.value = runCatchingCancellable {
				library.foldersIn(albumRef.server, root.id)
			}.getOrDefault(emptyList())
		}
	}

	fun onFolder(v: String) {
		_folder.value = v
	}

	/**
	 * Moves this album out of the uploads area into the shared library.
	 *
	 * The repository drops that server's mirror and bumps the library revision,
	 * so both listings are re-read; this only has to send the screen back, since
	 * the album it is showing is at a new id the moment this returns.
	 */
	fun promote() {
		if (_promoting.value || _deleting.value) return
		// Both halves of the destination are required by the server, and the
		// dialog keeps its Move button disabled until they are set - so these
		// guard against a caller that is not the dialog, rather than a case the
		// user can reach. Checked before anything is marked in flight.
		val root = _destRoot.value ?: return
		val folder = _folder.value.trim().ifBlank { return }
		_promoting.value = true
		_promoteError.value = null
		viewModelScope.launch {
			runCatchingCancellable {
				library.promoteAlbum(albumRef, root.id, folder)
			}.fold(
				onSuccess = { _promoted.value = true },
				// The server's own words. "Something with that name is already
				// in that folder" and "outside the library" need different
				// fixes, and one flat failure message told the user neither.
				onFailure = { _promoteError.value = "Could not move it: ${it.userMessage()}" },
			)
			_promoting.value = false
		}
	}

	/**
	 * Removes this upload from the server, files and all.
	 *
	 * Shares [promoted] with the move above rather than having a flag of its
	 * own: both mean "this album is not here any more, leave", and the screen
	 * does the same thing for each. [promoteError] is shared for the same
	 * reason - one dialog saying what the server said.
	 */
	fun deleteUpload() {
		if (_deleting.value || _promoting.value) return
		_deleting.value = true
		_promoteError.value = null
		viewModelScope.launch {
			runCatchingCancellable { library.deleteUpload(albumRef) }.fold(
				onSuccess = { _promoted.value = true },
				onFailure = {
					_promoteError.value = "Could not delete it: ${it.userMessage()}"
				},
			)
			_deleting.value = false
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

			// Started here too, and for the opposite reason: it is a request
			// the tracks must not wait on, but it reads an index and normally
			// answers before getAlbum does, so launching it in parallel is
			// what keeps its rows from appearing under the user's finger.
			val chapters = async {
				runCatchingCancellable { library.albumChapters(albumRef) }.getOrNull()
			}

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
				chapters.cancel()
				return@launch
			}

			// First of everything below, because it is the only one the user is
			// waiting to *hear*: a search tap must not be silent for as long as
			// the cover URL and the chapter index take.
			if (!autoPlayDone) {
				autoPlayDone = true
				ItemRef.decode(route.autoPlayRef.orEmpty())?.let { wanted ->
					val index = detail.songs.indexOfFirst { it.ref == wanted }
					if (index >= 0) _autoPlay.value = AutoPlay(index, route.autoPlayMs)
				}
			}

			// Tracks are visible from here on; each extra fills in as it
			// arrives and a failure costs only that one piece.
			covers.await()?.url(detail.album.coverArt, HERO_PX)
				?.let { url -> _extras.update { it.copy(heroUrl = url) } }

			// Before the notes, which can send the server off to MusicBrainz.
			chapters.await()?.takeIf { it.isNotEmpty() }
				?.let { found -> _extras.update { it.copy(chapters = found) } }

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
