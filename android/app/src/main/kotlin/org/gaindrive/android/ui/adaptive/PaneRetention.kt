package org.gaindrive.android.ui.adaptive

import androidx.compose.runtime.Composable
import androidx.compose.runtime.CompositionLocalProvider
import androidx.compose.runtime.DisposableEffect
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.Stable
import androidx.compose.runtime.remember
import androidx.compose.runtime.saveable.SaveableStateHolder
import androidx.compose.runtime.saveable.rememberSaveableStateHolder
import androidx.lifecycle.ViewModelStore
import androidx.lifecycle.ViewModelStoreOwner
import androidx.lifecycle.viewmodel.compose.LocalViewModelStoreOwner
import org.gaindrive.android.ui.Route

/**
 * What a pane keeps while it is off screen but still in the tab's path.
 *
 * A pane that has slid off the leading edge is [Pane.Gone], and a hidden pane
 * is not composed — which on a phone is *every* level but the deepest. So
 * without this, drilling in disposes the level above: its [PaneHost]'s
 * `NavController`, the back stack entry that host exists to provide, the view
 * model scoped to that entry and every `rememberSaveable` beneath it. Coming
 * back then built all of it again, and the symptom was the one a user
 * reports — the album list re-read from the server and scrolled back to the
 * top, having been sitting there a moment earlier.
 *
 * The path is not a history, so the fix is exactly as wide as the path: a
 * level that is still in [PaneStack.path] keeps its state, and a level the
 * path no longer holds loses it at once. Two halves, and both are needed —
 * either alone leaves half the symptom:
 *
 *  * **The saveable state**, so scroll offsets and anything else under a
 *    `rememberSaveable` survive. That includes `rememberNavController`'s own
 *    bundle, which is what makes the rebuilt host restore the *same* back
 *    stack entry rather than mint a new one — the id travels in the saved
 *    state, and the entry's id is what its view models are filed under.
 *  * **A [ViewModelStore] per pane**, so those view models are still there to
 *    be found. It is ours rather than the ambient one deliberately: the
 *    ambient owner is the tab's own entry, which would retain a view model
 *    for every artist ever opened until the tab was left, with nothing able
 *    to say which of them the user has finished with.
 *
 * Nothing here is navigation state — the path remains the only thing a tab
 * has, and this is state *hanging off* it, dropped as soon as a level leaves
 * the path for good.
 */
@Stable
class PaneRetention internal constructor(private val holder: SaveableStateHolder) {

	private val owners = mutableMapOf<String, PaneOwner>()

	/** Keys the strip has composed: one on a phone, up to three beside it. */
	private val composed = mutableSetOf<String>()

	/** Keys the path still holds. */
	private var live: Set<String> = emptySet()

	/**
	 * Draws one pane with everything it had last time it was on screen.
	 *
	 * Keyed on the route rather than on the level, so choosing a *different*
	 * album at the same level gets its own state instead of inheriting the
	 * previous one's scroll position — the same identity [PaneHost] keys its
	 * host on, and for the same reason.
	 */
	@Composable
	fun Retained(route: Route, content: @Composable () -> Unit) {
		val key = remember(route) { paneRouteKey(route) }
		val owner = owners.getOrPut(key) { PaneOwner() }

		// A pane leaves the strip for two different reasons — the window slid
		// past it, or the path dropped it — and only the second is a reason to
		// throw anything away. Which of the two this is cannot be known until
		// both have happened, so each says so and whichever is last decides.
		DisposableEffect(key) {
			composed += key
			onDispose {
				composed -= key
				if (key !in live) forget(key)
			}
		}

		holder.SaveableStateProvider(key) {
			CompositionLocalProvider(LocalViewModelStoreOwner provides owner) {
				content()
			}
		}
	}

	/** Called with the path's keys whenever it changes; see the effect above. */
	internal fun keepOnly(keys: Set<String>) {
		live = keys
		(owners.keys - keys - composed).forEach(::forget)
	}

	internal fun clear() {
		owners.values.forEach { it.viewModelStore.clear() }
		owners.clear()
		composed.clear()
	}

	/**
	 * Order-independent on purpose: `removeState` cancels the save when the
	 * pane is still composed and drops the saved bundle when it is not, so it
	 * is correct whether this runs before or after the provider's own disposal
	 * — which Compose does not promise either way round.
	 */
	private fun forget(key: String) {
		owners.remove(key)?.viewModelStore?.clear()
		holder.removeState(key)
	}
}

/** A pane's view models, and nothing else — see [PaneRetention]. */
private class PaneOwner : ViewModelStoreOwner {
	override val viewModelStore = ViewModelStore()
}

/**
 * [path] is the tab's whole path, and is read for one thing only: which levels
 * are still live. Pruning happens in an effect rather than in composition
 * because it clears view models, which is a side effect and must not run
 * twice for one change.
 *
 * The holder is remembered where the strip is, which is inside the tab's own
 * back stack entry — so the whole lot is saved with that entry and discarded
 * with it, exactly as the panes' state was before this existed.
 */
@Composable
fun rememberPaneRetention(path: List<Route>): PaneRetention {
	val holder = rememberSaveableStateHolder()
	val retention = remember(holder) { PaneRetention(holder) }

	val live: Set<String> = remember(path) { path.mapTo(mutableSetOf(), ::paneRouteKey) }
	LaunchedEffect(live) { retention.keepOnly(live) }
	DisposableEffect(retention) { onDispose { retention.clear() } }

	return retention
}
