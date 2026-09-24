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
import org.gaindrive.android.ui.playlists.PlaylistDetailScreen
import org.gaindrive.android.ui.playlists.PlaylistsScreen

/**
 * Playlists, and one playlist's tracks beside it where there is room.
 *
 * Two levels, so at any width above one pane this is a list and a detail and
 * never a third pane - which is what the two titles handed to [PaneStrip] say,
 * and why a wide window splits it in two rather than leaving an empty slot
 * inviting a choice that does not exist.
 */
@Composable
fun PlaylistsTab(stack: PaneStack) {
	PaneBackHandler(stack)

	PaneStrip(
		stack = stack,
		titles = listOf("Playlists", "Tracks"),
		waiting = { PaneWaiting("Choose a playlist to see its tracks") },
	) { route ->
		// Every route the tab can hold, declared once. Only the pane's own is
		// ever built - it is the start destination and nothing navigates.
		PaneHost(route) {
			composable<Route.Playlists> {
				PlaylistsScreen(
					onOpenPlaylist = { ref, name ->
						stack.show(1, Route.Playlist(ref.encode(), name))
					},
				)
			}
			composable<Route.Playlist> {
				// Read here rather than captured from outside: this lambda is the
				// graph builder, and NavHost remembers the graph on its identity.
				PlaylistDetailScreen(onBack = LocalPaneBack.current)
			}
		}
	}
}
