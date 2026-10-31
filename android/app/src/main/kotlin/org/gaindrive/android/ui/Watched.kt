package org.gaindrive.android.ui

import androidx.compose.runtime.compositionLocalOf
import androidx.lifecycle.ViewModel
import androidx.lifecycle.viewModelScope
import dagger.hilt.android.lifecycle.HiltViewModel
import kotlinx.coroutines.flow.SharedFlow
import kotlinx.coroutines.flow.SharingStarted
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.map
import kotlinx.coroutines.flow.stateIn
import org.gaindrive.android.data.WatchOverride
import org.gaindrive.android.data.WatchStore
import org.gaindrive.android.data.model.ItemRef
import org.gaindrive.android.data.model.Song
import javax.inject.Inject

/**
 * The watched mark and resume point every row, sheet and album header reads,
 * over what its own model was loaded with. See [WatchStore].
 */
class Watched(
	private val overrides: Map<ItemRef, WatchOverride> = emptyMap(),
	private val store: WatchStore? = null,
) {
	fun isWatched(song: Song): Boolean =
		overrides[song.ref]?.watched ?: (song.watchedAt != null)

	/** How far in, 0..1; 0 for a finished item, as the web client draws it. */
	fun progress(song: Song): Float {
		if (!song.isResumable || isWatched(song) || song.duration <= 0) return 0f
		val at = overrides[song.ref]?.positionMs ?: song.bookmarkMs
		return (at / 1000f / song.duration).coerceIn(0f, 1f)
	}

	/** Where to start [song] to pick it up again, in milliseconds. */
	fun resumeMs(song: Song): Long =
		if (isWatched(song)) 0L else overrides[song.ref]?.positionMs ?: song.bookmarkMs

	fun setWatched(songs: List<Song>, watched: Boolean) {
		store?.setWatched(songs, watched)
	}
}

/** Ambient for the same reason as [LocalStars]: every track row wants it. */
val LocalWatched = compositionLocalOf { Watched() }

@HiltViewModel
class WatchedViewModel @Inject constructor(
	store: WatchStore,
) : ViewModel() {

	val state: StateFlow<Watched> =
		store.overrides.map { Watched(it, store) }.stateIn(
			scope = viewModelScope,
			started = SharingStarted.WhileSubscribed(5_000),
			initialValue = Watched(store = store),
		)

	/** Refusals, for the shell to toast: the sheet that asked has closed by then. */
	val errors: SharedFlow<String> = store.errors
}
