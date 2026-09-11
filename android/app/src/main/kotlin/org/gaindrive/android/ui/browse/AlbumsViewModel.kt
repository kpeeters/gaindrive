package org.gaindrive.android.ui.browse

import androidx.lifecycle.SavedStateHandle
import androidx.lifecycle.ViewModel
import androidx.lifecycle.viewModelScope
import androidx.navigation.toRoute
import dagger.hilt.android.lifecycle.HiltViewModel
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.SharingStarted
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.asStateFlow
import kotlinx.coroutines.flow.combine
import kotlinx.coroutines.flow.stateIn
import kotlinx.coroutines.flow.update
import kotlinx.coroutines.launch
import org.gaindrive.android.data.CoverUrls
import org.gaindrive.android.data.LibraryRepository
import org.gaindrive.android.data.ServerFailure
import org.gaindrive.android.data.SettingsStore
import org.gaindrive.android.data.model.Album
import org.gaindrive.android.data.model.AlbumSort
import org.gaindrive.android.data.model.ArtistInfo
import org.gaindrive.android.data.model.ItemRef
import org.gaindrive.android.data.model.LibrarySection
import org.gaindrive.android.data.model.comparator
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
	private val settings: SettingsStore,
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

	/** Passed down to each album, and only ever true when browsing Uploads. */
	val fromUploads: Boolean = route.fromUploads

	/**
	 * True when this "artist" is a section of a categories root. It has no
	 * portrait and no biography — `is_category_folder()` on the server refuses
	 * the lookup — so the header draws neither, and neither is asked for.
	 */
	val isCategory: Boolean = route.fromCategories

	private val _albums = MutableStateFlow<Load<List<AlbumUi>>>(Load.Loading)

	/**
	 * Which section this listing was drilled in from, keying the sort
	 * preference: a discography wants a different order than a film category.
	 * Derived from the route flags rather than from any stored "current
	 * section", so the key always matches the listing on screen.
	 */
	private val section = when {
		route.fromUploads -> LibrarySection.UPLOADS
		route.fromCategories -> LibrarySection.CATEGORIES
		else -> LibrarySection.ARTISTS
	}

	val sort: StateFlow<AlbumSort> = settings.albumSort(section)
		.stateIn(viewModelScope, SharingStarted.Lazily, AlbumSort.DEFAULT)

	/**
	 * The listing, in the order chosen for this library slice.
	 *
	 * Sorted here rather than in [loadAlbums] so changing the order costs
	 * nothing on the wire: the whole list is already in hand, and re-reading it
	 * to reorder it would put a spinner over a decision that should be
	 * instant. It also fixes the order of a *merged* artist, whose albums
	 * arrive as one server's list concatenated with the next's.
	 */
	val state: StateFlow<Load<List<AlbumUi>>> =
		combine(_albums, sort) { load, order ->
			if (load is Load.Ready) {
				Load.Ready(
					load.value.sortedWith(compareBy<AlbumUi, Album>(order.comparator) { it.album })
				)
			} else {
				load
			}
		}.stateIn(viewModelScope, SharingStarted.Lazily, Load.Loading)

	fun setSort(order: AlbumSort) =
		viewModelScope.launch { settings.setAlbumSort(section, order) }

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
		_albums.value = Load.Loading
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
					_albums.value = Load.Failed(merged.failures.first().message)
				} else {
					_albums.value = Load.Ready(merged.items)
				}
				_failures.value = merged.failures
			},
			onFailure = {
				_albums.value = Load.Failed(it.userMessage())
				_failures.value = emptyList()
			},
		)
		_isRefreshing.value = false
	}

	/**
	 * Fills in the header as it arrives: the portrait URL first, then the
	 * biography, which may involve a lookup.
	 *
	 * Note that the portrait URL arriving is not the portrait arriving. Both
	 * halves of this header come from a server-side lookup now — `getCoverArt`
	 * answers 404 for an artist it has not resolved yet, and only the request
	 * itself puts them at the front of the queue. Re-running this function
	 * cannot recover from that, because the URL it computes is identical and
	 * [ArtistHeaderUi] is a data class, so the state never changes and nothing
	 * recomposes. The retry lives in `ArtistAvatar`, which is the only thing
	 * that can see whether the image actually loaded.
	 */
	private fun loadHeader() = viewModelScope.launch {
		// Nothing to fill in for a section, and nothing to wait for: the
		// portrait would 404 for as long as ArtistAvatar kept retrying it, and
		// getArtistInfo2 answers empty by design.
		if (isCategory) return@launch

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
