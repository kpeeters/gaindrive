package org.gaindrive.android.ui.player

import androidx.lifecycle.ViewModel
import androidx.lifecycle.viewModelScope
import dagger.hilt.android.lifecycle.HiltViewModel
import kotlinx.coroutines.ExperimentalCoroutinesApi
import kotlinx.coroutines.flow.SharingStarted
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.distinctUntilChanged
import kotlinx.coroutines.flow.flatMapLatest
import kotlinx.coroutines.flow.flow
import kotlinx.coroutines.flow.map
import kotlinx.coroutines.flow.stateIn
import org.gaindrive.android.data.ChapterTracks
import org.gaindrive.android.data.model.ChapterList
import org.gaindrive.android.playback.PlayerConnection
import javax.inject.Inject

/**
 * The markers inside whatever is playing.
 *
 * Read through `getChapters`, which opens the file, rather than through the
 * album index: a jump list has to be right about a sidecar somebody edited a
 * moment ago, and it is the only endpoint that can see a video's own container
 * chapters at all.
 *
 * `flatMapLatest` over a distinct current-item ref is the whole staleness
 * story, and it is better than the guard-after-the-await the web client needs:
 * a queue advance *cancels* the request in flight rather than letting it land
 * and be discarded, so one film's songs can never be drawn over another's.
 *
 * Deliberately not folded into [org.gaindrive.android.playback.PlayerState],
 * which is rebuilt wholesale every 500 ms from the controller: an asynchronous
 * list threaded through that would have to be held in [PlayerConnection] for a
 * single screen's benefit.
 */
@OptIn(ExperimentalCoroutinesApi::class)
@HiltViewModel
class VideoChaptersViewModel @Inject constructor(
	private val chapters: ChapterTracks,
	player: PlayerConnection,
) : ViewModel() {

	val state: StateFlow<ChapterList> = player.state
		.map { it.current?.ref }
		.distinctUntilChanged()
		.flatMapLatest { ref ->
			flow {
				// Cleared first, so the previous item's markers are never on
				// screen beside the new item's title.
				emit(ChapterList())
				if (ref != null) emit(chapters.chaptersFor(ref))
			}
		}
		.stateIn(viewModelScope, SharingStarted.WhileSubscribed(5_000), ChapterList())
}
