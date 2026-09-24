package org.gaindrive.android.ui.recents

import androidx.lifecycle.ViewModel
import androidx.lifecycle.viewModelScope
import dagger.hilt.android.lifecycle.HiltViewModel
import kotlinx.coroutines.Job
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.SharingStarted
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.asStateFlow
import kotlinx.coroutines.flow.distinctUntilChanged
import kotlinx.coroutines.flow.stateIn
import kotlinx.coroutines.launch
import org.gaindrive.android.data.LibraryRepository
import org.gaindrive.android.data.ServerFailure
import org.gaindrive.android.data.ServerSection
import org.gaindrive.android.data.ServerSelection
import org.gaindrive.android.data.model.BrowseScope
import org.gaindrive.android.data.model.ServerConfig
import org.gaindrive.android.data.model.ServerId
import org.gaindrive.android.data.model.Song
import org.gaindrive.android.net.runCatchingCancellable
import org.gaindrive.android.net.userMessage
import org.gaindrive.android.playback.PlayerConnection
import org.gaindrive.android.ui.Load
import org.gaindrive.android.ui.SongUi
import javax.inject.Inject

@HiltViewModel
class RecentsViewModel @Inject constructor(
	private val library: LibraryRepository,
	private val player: PlayerConnection,
	private val selection: ServerSelection,
) : ViewModel() {

	/**
	 * Grouped by server, never interleaved. Each server only knows what was
	 * played against it, so ordering them together by timestamp would imply a
	 * completeness that does not exist.
	 */
	private val _state = MutableStateFlow<Load<List<ServerSection<SongUi>>>>(Load.Loading)
	val state: StateFlow<Load<List<ServerSection<SongUi>>>> = _state.asStateFlow()

	private val _failures = MutableStateFlow<List<ServerFailure>>(emptyList())
	val failures: StateFlow<List<ServerFailure>> = _failures.asStateFlow()

	/** True only for a user-initiated pull, which drives the pull indicator. */
	private val _isRefreshing = MutableStateFlow(false)
	val isRefreshing: StateFlow<Boolean> = _isRefreshing.asStateFlow()

	private var scope: BrowseScope = BrowseScope.AllServers
	private var loadJob: Job? = null

	init {
		viewModelScope.launch {
			selection.browse.distinctUntilChanged().collect { selected ->
				scope = selected.scope
				startLoad(clearFirst = true)
			}
		}
	}

	/** Retry after a failure: there is nothing worth keeping on screen. */
	fun load() = startLoad(clearFirst = true)

	fun refresh() {
		_isRefreshing.value = true
		startLoad(clearFirst = false)
	}

	fun dismissFailures() {
		_failures.value = emptyList()
	}

	/**
	 * Plays [song] with the rest of its album queued behind it, which is what
	 * the web client does when a track is opened from a listing - playing a
	 * lone track and falling silent at the end of it is not what picking
	 * something out of a history means.
	 *
	 * The album is fetched here rather than on the album screen the caller is
	 * navigating to: that screen is a separate destination with its own load,
	 * and waiting for it would leave the tap silent for as long as it takes.
	 */
	fun playInAlbumContext(song: Song) {
		viewModelScope.launch {
			val albumRef = song.albumRef
			val album = albumRef?.let {
				runCatchingCancellable { library.albumDetail(it) }.getOrNull()
			}
			val index = album?.songs?.indexOfFirst { it.ref == song.ref } ?: -1
			// No album, or a track the album no longer lists: play the one
			// track rather than nothing at all.
			if (album == null || index < 0) player.play(listOf(song), 0)
			else player.play(album.songs, index)
		}
	}

	private fun startLoad(clearFirst: Boolean) {
		loadJob?.cancel()
		loadJob = viewModelScope.launch {
			if (clearFirst) _state.value = Load.Loading
			if (selection.hasNoServers()) {
				_state.value = Load.Failed("No server configured. Add one in Settings.")
				_isRefreshing.value = false
				return@launch
			}

			runCatchingCancellable {
				val covers = library.coverUrls()
				library.recentSongs(scope, RECENT_SIZE).map { sections ->
					sections.map { section ->
						ServerSection(
							server = section.server,
							items = section.items.map {
								SongUi(it, covers.url(it.coverArt, COVER_PX))
							},
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
	}

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
		const val RECENT_SIZE = 50

		/** Matches the 40dp thumbnail at roughly 3x density. */
		const val COVER_PX = 144
	}
}
