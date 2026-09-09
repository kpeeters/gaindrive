package org.gaindrive.android.ui.fetch

import androidx.lifecycle.ViewModel
import androidx.lifecycle.viewModelScope
import dagger.hilt.android.lifecycle.HiltViewModel
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.SharingStarted
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.combine
import kotlinx.coroutines.flow.stateIn
import kotlinx.coroutines.flow.update
import org.gaindrive.android.data.FetchJobRef
import org.gaindrive.android.data.FetchMonitor
import org.gaindrive.android.data.FetchStatus
import org.gaindrive.android.data.model.FetchState
import javax.inject.Inject

/** What the strip in the app's bottom bar draws, or null to draw nothing. */
data class FetchStripState(
	val jobs: List<FetchJobRef>,
	val dismissed: Set<String>,
) {
	private val shown: List<FetchJobRef>
		get() = jobs.filterNot { it.job.id in dismissed }

	val live: List<FetchJobRef> get() = shown.filter { it.state.isLive }

	/** The one actually moving, which is what a determinate bar can describe. */
	val moving: FetchJobRef?
		get() = live.firstOrNull {
			it.state == FetchState.RUNNING || it.state == FetchState.SCANNING
		}

	/**
	 * A finished job worth still saying something about.
	 *
	 * Only while nothing is live: a fetch that has just ended beside one still
	 * running is not news, and the running one is what the strip should be
	 * reporting.
	 */
	val notice: FetchJobRef?
		get() = if (live.isNotEmpty()) null else shown.firstOrNull {
			it.state == FetchState.ERROR ||
				(it.state == FetchState.DONE &&
					System.currentTimeMillis() / 1000 - it.job.finished < NOTICE_S)
		}

	val showing: Boolean get() = live.isNotEmpty() || notice != null

	private companion object {
		/**
		 * How long a finished fetch keeps the strip. Long enough to be seen by
		 * someone who was looking at another screen when it landed, short enough
		 * that it is gone before it becomes furniture. A failure is exempt: it
		 * needs a decision, and nothing else in the app will ever mention it.
		 */
		const val NOTICE_S = 120
	}
}

/**
 * The shell's view of [FetchMonitor], and — because collecting the monitor is
 * what makes it poll — the thing that decides when it runs at all.
 *
 * That is the point of putting it here rather than leaving the monitor to the
 * fetch panel. The shell is composed for as long as the app is on screen, so
 * the app's own lifecycle becomes the poll's, and coming back to the app is
 * what re-runs the monitor's first sweep. A fetch is then never invisible: it
 * was, and that is the bug this exists to close.
 *
 * Not folded into `LocalAvailability`. That ambient answers whether a track can
 * play, which every row wants and no screen has an opinion about; this has one
 * consumer in the shell and one on the uploads listing, and making it ambient
 * would spend a composition local to save two injections.
 */
@HiltViewModel
class FetchStatusViewModel @Inject constructor(
	private val monitor: FetchMonitor,
) : ViewModel() {

	// Per process, not persisted: dismissing a notice is a statement about this
	// glance at the screen, and the server forgets the job within the quarter
	// hour anyway.
	private val dismissed = MutableStateFlow(emptySet<String>())

	val state: StateFlow<FetchStripState> =
		combine(monitor.status, dismissed) { status: FetchStatus, hidden ->
			FetchStripState(status.jobs, hidden)
		}.stateIn(
			viewModelScope,
			SharingStarted.WhileSubscribed(SUBSCRIBER_GRACE_MS),
			FetchStripState(emptyList(), emptySet()),
		)

	fun dismiss(id: String) = dismissed.update { it + id }

	private companion object {
		/** What a rotation must not look like, as elsewhere in the shell. */
		const val SUBSCRIBER_GRACE_MS = 5_000L
	}
}
