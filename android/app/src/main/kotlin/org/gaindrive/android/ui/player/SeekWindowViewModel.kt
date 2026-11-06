package org.gaindrive.android.ui.player

import androidx.lifecycle.ViewModel
import androidx.lifecycle.viewModelScope
import dagger.hilt.android.lifecycle.HiltViewModel
import kotlinx.coroutines.ExperimentalCoroutinesApi
import kotlinx.coroutines.flow.SharingStarted
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.combine
import kotlinx.coroutines.flow.distinctUntilChanged
import kotlinx.coroutines.flow.flatMapLatest
import kotlinx.coroutines.flow.flow
import kotlinx.coroutines.flow.map
import kotlinx.coroutines.flow.stateIn
import org.gaindrive.android.data.ChapterTracks
import org.gaindrive.android.data.SettingsStore
import org.gaindrive.android.data.model.Chapter
import org.gaindrive.android.playback.PlayerConnection
import javax.inject.Inject

/**
 * The markers the seekbar confines itself to: those of a film played as audio
 * only, while "Seekbar is local to chapters" is on, and none otherwise.
 *
 * Asked only in that case, so an ordinary song costs no `getChapters` call.
 * The same staleness story as [VideoChaptersViewModel]: a new item cancels the
 * request for the old one.
 */
@OptIn(ExperimentalCoroutinesApi::class)
@HiltViewModel
class SeekWindowViewModel @Inject constructor(
	private val chapters: ChapterTracks,
	player: PlayerConnection,
	settings: SettingsStore,
) : ViewModel() {

	val chapterList: StateFlow<List<Chapter>> = combine(
		player.state.map { it.current?.takeIf { now -> now.audioOnlyVideo }?.ref },
		settings.seekbarPerChapter,
	) { ref, perChapter -> if (perChapter) ref else null }
		.distinctUntilChanged()
		.flatMapLatest { ref ->
			flow {
				emit(emptyList())
				if (ref != null) emit(chapters.chaptersFor(ref).chapters)
			}
		}
		.stateIn(viewModelScope, SharingStarted.WhileSubscribed(5_000), emptyList())
}
