package org.gaindrive.android.ui.tabs

import androidx.compose.runtime.Composable
import androidx.navigation.compose.composable
import org.gaindrive.android.ui.Route
import org.gaindrive.android.ui.adaptive.LocalPaneBack
import org.gaindrive.android.ui.adaptive.Pane
import org.gaindrive.android.ui.adaptive.PaneBackHandler
import org.gaindrive.android.ui.adaptive.PaneHost
import org.gaindrive.android.ui.adaptive.PaneStack
import org.gaindrive.android.ui.adaptive.PaneStrip
import org.gaindrive.android.ui.adaptive.PaneWaiting
import org.gaindrive.android.ui.adaptive.leadingWindow
import org.gaindrive.android.ui.browse.AlbumDetailScreen
import org.gaindrive.android.ui.browse.AlbumsScreen
import org.gaindrive.android.ui.recents.RecentsScreen

/**
 * Recently played, the album a row opens, and that album's artist between
 * the two — which is what the web client shows for the same tap, its
 * `viewTracks` back-filling pane 1 with the artist's albums.
 *
 * The middle level cannot be built when the row is tapped: a song row
 * carries the artist's *name* and nothing addressable, and the artist
 * folder id arrives only with the album detail. So the tap opens the album
 * directly under the list, exactly as before, and the album pane reports
 * the artist upward once it has loaded — [PaneStack.insert] then grows the
 * path a level *under* the reader, without moving what they are looking at.
 */
@Composable
fun RecentsTab(stack: PaneStack) {
	PaneBackHandler(stack)

	PaneStrip(
		stack = stack,
		titles = listOf("Recently played", "Albums", "Album"),
		// A Waiting third pane is only honest once the middle level actually
		// holds the artist's albums; before the back-fill it would invite a
		// choice from a list that does not exist yet. Blank, not Gone: the
		// pane must keep its width, it just must not ask for anything.
		slots = { depth, panes, levels ->
			val base = leadingWindow(depth, panes, levels)
			if (base.extra is Pane.Waiting && stack.path.getOrNull(1) !is Route.Albums) {
				base.copy(extra = Pane.Blank)
			} else {
				base
			}
		},
		waiting = { level ->
			PaneWaiting(
				if (level == 1) "Choose something you played to open its album"
				else "Choose an album to see its tracks"
			)
		},
	) { route ->
		PaneHost(route) {
			composable<Route.Recents> {
				RecentsScreen(
					onOpenAlbum = { ref, title ->
						stack.show(1, Route.Album(ref.encode(), title))
					},
				)
			}
			composable<Route.Albums> {
				AlbumsScreen(
					onBack = LocalPaneBack.current,
					onOpenAlbum = { ref, title, fromUploads ->
						stack.show(2, Route.Album(ref.encode(), title, fromUploads))
					},
				)
			}
			composable<Route.Album> {
				AlbumDetailScreen(
					onBack = LocalPaneBack.current,
					// Promote is only offered on the Library tab's Uploads path,
					// so nothing here can reach it; back to the list either way.
					onPromoted = { stack.reset() },
					// Guarded on this album still sitting directly under the
					// recents list: a slow load must not rewrite a path the
					// user has already moved on from, and after the insert the
					// depth alone fails the test, so it fires once.
					onArtistKnown = { artistRef, artistName ->
						if (stack.depth == 1 && stack.path.getOrNull(1) == route) {
							stack.insert(1, Route.Albums(artistRef.encode(), artistName))
						}
					},
				)
			}
		}
	}
}
