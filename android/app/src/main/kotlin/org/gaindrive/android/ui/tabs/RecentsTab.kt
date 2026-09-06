package org.gaindrive.android.ui.tabs

import androidx.compose.runtime.Composable
import androidx.navigation.compose.composable
import org.gaindrive.android.ui.Route
import org.gaindrive.android.ui.adaptive.LocalPaneBack
import org.gaindrive.android.ui.adaptive.PaneBackHandler
import org.gaindrive.android.ui.adaptive.PaneHost
import org.gaindrive.android.ui.adaptive.PaneStack
import org.gaindrive.android.ui.adaptive.PaneStrip
import org.gaindrive.android.ui.adaptive.PaneWaiting
import org.gaindrive.android.ui.browse.AlbumDetailScreen
import org.gaindrive.android.ui.recents.RecentsScreen

/**
 * Recently played, and the album a row opens beside it.
 *
 * Two levels, not three: a recent row names a song, and opening it goes
 * straight to that song's album. The same screen sits at level 2 in the
 * Library tab, which is exactly why a level is a position in *this* tab's
 * path rather than a property of the screen.
 */
@Composable
fun RecentsTab(stack: PaneStack) {
	PaneBackHandler(stack)

	PaneStrip(
		stack = stack,
		titles = listOf("Recently played", "Album"),
		waiting = { PaneWaiting("Choose something you played to open its album") },
	) { route ->
		PaneHost(route) {
			composable<Route.Recents> {
				RecentsScreen(
					onOpenAlbum = { ref, title ->
						stack.show(1, Route.Album(ref.encode(), title))
					},
				)
			}
			composable<Route.Album> {
				AlbumDetailScreen(
					onBack = LocalPaneBack.current,
					// Promote is only offered on the Library tab's Uploads path,
					// so nothing here can reach it; back to the list either way.
					onPromoted = { stack.reset() },
				)
			}
		}
	}
}
