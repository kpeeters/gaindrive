package org.gaindrive.android.ui.adaptive

import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.runtime.Composable
import androidx.compose.runtime.key
import androidx.compose.runtime.remember
import androidx.compose.ui.Modifier
import androidx.navigation.NavGraphBuilder
import androidx.navigation.compose.NavHost
import androidx.navigation.compose.rememberNavController
import kotlinx.serialization.decodeFromString
import kotlinx.serialization.encodeToString
import kotlinx.serialization.json.Json
import org.gaindrive.android.ui.Route

/**
 * One pane, holding one screen, in a navigation host of its own.
 *
 * **The host is not navigation.** It is never navigated — the tab's
 * [PaneStack] is the back stack — and it exists because a pane needs a
 * [androidx.navigation.NavBackStackEntry] that is *resumed*. Two reasons, and
 * the second is the one that is easy to miss:
 *
 *  * The screen's view model is scoped to that entry, and reads its arguments
 *    from the entry's `SavedStateHandle` through `toRoute`. Every arg-carrying
 *    view model in the app does this, so a pane without an entry would mean
 *    rewriting all of them.
 *  * `NavController` forces every entry below the top of its stack to
 *    `CREATED`, and `collectAsStateWithLifecycle` stops collecting below
 *    `STARTED`. So two panes taken from *one* controller would not merely lack
 *    arguments — the one behind would silently stop updating, showing a list
 *    frozen at whatever it held when the user drilled in. N panes on screen at
 *    once therefore need N controllers, one each, and this is that one.
 *
 * Two things follow that must stay true. **Never navigate this host**: it is
 * one entry deep, which is also what stops it claiming the back gesture, since
 * `NavController` only enables its own back callback above one entry — so back
 * reaches [PaneBackHandler] and moves the whole strip, as the web client's
 * back link does. And **the key is the route**, so choosing a different album
 * throws the host away and builds a fresh entry with the new arguments rather
 * than trying to re-argue an existing one.
 *
 * A host is also thrown away when the *window* slides past its pane, which on
 * a phone is every drill-down — and there it must come back with everything it
 * had. That is [PaneRetention]'s job, not this one's: it provides the saveable
 * state `rememberNavController` restores from and the view model store the
 * restored entry's models are found in, so the host rebuilt here is the same
 * host rather than a new one wearing the same route.
 *
 * [destinations] may declare every route the tab uses; only [route] is ever
 * built, because it is the start destination and nothing navigates. One graph
 * per tab rather than one per pane is simply less to keep in step.
 */
@Composable
fun PaneHost(route: Route, destinations: NavGraphBuilder.() -> Unit) {
	val paneKey = remember(route) { paneRouteKey(route) }
	key(paneKey) {
		val nav = rememberNavController()
		NavHost(
			navController = nav,
			startDestination = route,
			modifier = Modifier.fillMaxSize(),
			builder = destinations,
		)
	}
}

/**
 * A route as a string, for [PaneHost]'s key and for [PaneStack]'s saved state.
 *
 * One encoding for both, because the two answer the same question — are these
 * the same destination — and two answers that disagreed would be a pane that
 * did not rebuild when the path said it should.
 */
fun paneRouteKey(route: Route): String = PaneJson.encodeToString(route)

/** The inverse. Throws on anything this build did not write; see the saver. */
fun paneRouteOf(key: String): Route = PaneJson.decodeFromString(key)

private val PaneJson = Json
