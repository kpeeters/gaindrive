package org.gaindrive.android.ui.adaptive

import android.util.Log
import androidx.activity.compose.BackHandler
import androidx.compose.runtime.Composable
import androidx.compose.runtime.MutableState
import androidx.compose.runtime.Stable
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.saveable.Saver
import androidx.compose.runtime.saveable.listSaver
import androidx.compose.runtime.saveable.rememberSaveable
import androidx.compose.runtime.setValue
import androidx.lifecycle.compose.LifecycleResumeEffect
import org.gaindrive.android.ui.Route

/**
 * One tab's path: the route of every level from the tab's own root down to
 * whatever is being read. `path[0]` is the root and `path.last()` is the
 * current destination, so [depth] is `paneNav.depth` with the routes attached.
 *
 * **This is the only state a tab has.** Which panes are on screen, how many,
 * where the back arrow goes and what each pane draws are all derived from it
 * per frame - see [PaneStrip]. That is what makes a rotation, a fold or a
 * split-screen drag change no state at all: the pane count recomputes and
 * panes appear or disappear, while the path they were drawn from is untouched.
 * Going from three panes to one and back again therefore loses nothing, which
 * a "current route" could not promise.
 *
 * The invariant that makes back trivial is in [show]: a level is *replaced*,
 * never stacked onto. It is the web client's rule - rendering a pane clears
 * every pane deeper than it - and it means [back] can never be a move the user
 * cannot see.
 */
@Stable
class PaneStack internal constructor(private val state: MutableState<List<Route>>) {

	val path: List<Route> get() = state.value

	/** The level being read; the rightmost visible pane holds it. */
	val depth: Int get() = state.value.lastIndex

	/**
	 * Put [route] at [level], discarding anything deeper.
	 *
	 * Never an append past `level`, which is what keeps the path a path rather
	 * than a history: choosing a different album at the same level replaces the
	 * one beside it instead of burying it, so [back] always moves the strip.
	 */
	fun show(level: Int, route: Route) {
		state.value = state.value.take(level) + route
	}

	/**
	 * Put [route] at [level], keeping everything deeper - the web client's
	 * pane-1 back-fill (`viewTracks` filling the albums pane), for a level
	 * whose content only becomes known after the level below it has loaded.
	 * [show] is the wrong tool for that: it discards the deeper levels, which
	 * here are the very pane the user is reading.
	 *
	 * It inserts rather than replaces, so the caller must check the level does
	 * not already hold what is being added - see RecentsTab, whose guard is
	 * also what stops a late load rewriting a path the user has moved on from.
	 */
	fun insert(level: Int, route: Route) {
		state.value = state.value.take(level) + route + state.value.drop(level)
	}

	/** One level up. Nothing to do at the root - the tab itself is the floor. */
	fun back() {
		if (state.value.size > 1) state.value = state.value.dropLast(1)
	}

	/** Straight back to the tab's root, for the tap-the-current-tab gesture. */
	fun reset() {
		if (state.value.size > 1) state.value = listOf(state.value.first())
	}
}

/**
 * [initial] is the whole path, not just the root, so a tab can start somewhere
 * other than its own list - which is how a first run opens on Settings →
 * Servers with no conditional start destination anywhere.
 *
 * A lambda, and evaluated once: after process death the saved path wins, and a
 * value recomputed on every composition would be a first-run check that fired
 * again over a restored path.
 *
 * Held by the shell rather than by the tab, which is what makes a tab's path
 * survive a switch away from it without depending on the navigation library's
 * `saveState`/`restoreState` bookkeeping at all.
 */
@Composable
fun rememberPaneStack(initial: () -> List<Route>): PaneStack {
	val state = rememberSaveable(stateSaver = routeListSaver) { mutableStateOf(initial()) }
	return remember { PaneStack(state) }
}

/**
 * Back for the whole strip: one level of the path, whichever pane is showing
 * it. There is deliberately one of these per tab and none per pane - see
 * [PaneHost] on why a pane's own nav host must not claim the gesture.
 *
 * **Call it from the tab, outside the panes.** The guard is that only a tab
 * actually on screen may claim back, and it is needed because the shell
 * cross-fades: two tabs are composed at once mid-transition, and two enabled
 * `BackHandler`s is last-registered-wins, so without it a back press during
 * those few hundred milliseconds pops the wrong tab's path. Inside a pane the
 * effect below would answer for the pane's own nav entry, which is always
 * resumed, and the guard would be a no-op that looked like one.
 */
@Composable
fun PaneBackHandler(stack: PaneStack) {
	var onScreen by remember { mutableStateOf(false) }
	LifecycleResumeEffect(Unit) {
		onScreen = true
		onPauseOrDispose { onScreen = false }
	}
	BackHandler(enabled = onScreen && stack.path.size > 1) { stack.back() }
}

/**
 * Saved as the same JSON [PaneHost] keys its panes on, so a route has one
 * encoding rather than two that could disagree about whether two paths are the
 * same.
 *
 * Restoring is guarded because the discriminator is a class name: a saved
 * bundle written by a build that spelled a route differently decodes to
 * nothing, and dropping the path lands the user on the tab's own list, which
 * is somewhere. Throwing here would instead crash the app on the way back from
 * process death, which is the worst moment to do it. Logged rather than
 * swallowed - a path that will not decode is a real fault, just not one worth
 * a crash.
 */
private val routeListSaver: Saver<List<Route>, Any> = listSaver(
	save = { path -> path.map(::paneRouteKey) },
	restore = { keys ->
		try {
			keys.map(::paneRouteOf)
		} catch (e: Exception) {
			Log.w(TAG, "discarding an unreadable pane path: ${e.message}")
			null
		}
	},
)

private const val TAG = "GainDrivePanes"
