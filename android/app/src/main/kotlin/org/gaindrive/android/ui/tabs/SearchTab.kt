package org.gaindrive.android.ui.tabs

import androidx.compose.runtime.Composable
import androidx.navigation.compose.composable
import org.gaindrive.android.data.model.ItemRef
import org.gaindrive.android.ui.Route
import org.gaindrive.android.ui.adaptive.LocalPaneBack
import org.gaindrive.android.ui.adaptive.Pane
import org.gaindrive.android.ui.adaptive.PaneBackHandler
import org.gaindrive.android.ui.adaptive.PaneHost
import org.gaindrive.android.ui.adaptive.PaneStack
import org.gaindrive.android.ui.adaptive.PaneStrip
import org.gaindrive.android.ui.adaptive.PaneWaiting
import org.gaindrive.android.ui.adaptive.searchWindow
import org.gaindrive.android.ui.browse.AlbumDetailScreen
import org.gaindrive.android.ui.browse.AlbumsScreen
import org.gaindrive.android.ui.search.SearchScreen

/**
 * Search results, and whatever a hit opens beside them.
 *
 * Three levels, because an artist hit opens that artist's albums and an album
 * from there opens its tracks — but the *results* are what must stay on
 * screen, which is why this is the one tab that does not use the ordinary
 * window rule. See [searchWindow].
 *
 * An album hit and a chapter hit both land at level 1, and an album opened
 * from an artist hit lands at level 2. The same screen at two levels is
 * ordinary here: a level is a position in this tab's path, not a property of
 * the screen.
 */
@Composable
fun SearchTab(stack: PaneStack) {
	PaneBackHandler(stack)

	PaneStrip(
		stack = stack,
		titles = listOf("Search results", "Albums", "Tracks"),
		// searchWindow knows how deep the tab goes but not what is *at* each
		// level, and here that matters: an album hit lands at level 1 and is a
		// leaf, while an artist hit lands there as a list with albums below it.
		// Without this, a wide window beside an album hit offers a third pane
		// asking for an album — of a pane that is already one.
		slots = { depth, panes, levels ->
			val base = searchWindow(depth, panes, levels)
			if (base.extra is Pane.Waiting && stack.path.getOrNull(1) !is Route.Albums) {
				base.copy(extra = Pane.Gone)
			} else {
				base
			}
		},
		waiting = { level ->
			PaneWaiting(
				if (level == 1) "Choose a result to open it"
				else "Choose an album to see its tracks"
			)
		},
	) { route ->
		PaneHost(route) {
			composable<Route.Search> {
				SearchScreen(
					onOpenArtist = { refs, name ->
						stack.show(1, Route.Albums(ItemRef.encodeAll(refs), name))
					},
					onOpenAlbum = { ref, title ->
						stack.show(1, Route.Album(ref.encode(), title))
					},
					// A marker is played by opening the recording's album and
					// starting it partway in — see Route.Album for why that beats
					// playing it from here. A hit inside a film then lands on the
					// video screen by itself, through the same rule that sends any
					// video there, with the results left beside it.
					onOpenChapter = { hit ->
						hit.albumRef?.let { album ->
							stack.show(
								1,
								Route.Album(
									albumRef = album.encode(),
									albumTitle = hit.albumTitle,
									autoPlayRef = hit.songRef.encode(),
									autoPlayMs = hit.startMs,
								),
							)
						}
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
					// Nothing reached from search is on the Uploads path, so
					// promote is never offered; back to the results either way.
					onPromoted = { stack.reset() },
				)
			}
		}
	}
}
