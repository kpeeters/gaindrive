package org.gaindrive.android.ui.tabs

import androidx.compose.runtime.Composable
import androidx.navigation.compose.composable
import org.gaindrive.android.data.model.ItemRef
import org.gaindrive.android.data.model.LibrarySection
import org.gaindrive.android.ui.Route
import org.gaindrive.android.ui.adaptive.LocalPaneBack
import org.gaindrive.android.ui.adaptive.PaneBackHandler
import org.gaindrive.android.ui.adaptive.PaneHost
import org.gaindrive.android.ui.adaptive.PaneStack
import org.gaindrive.android.ui.adaptive.PaneStrip
import org.gaindrive.android.ui.adaptive.PaneWaiting
import org.gaindrive.android.ui.browse.AlbumDetailScreen
import org.gaindrive.android.ui.browse.AlbumsScreen
import org.gaindrive.android.ui.browse.ArtistsScreen

/**
 * The library: artists, an artist's albums, and an album's tracks.
 *
 * The only three-level tab, and the one the pane strip exists for — this is
 * the web client's `#pane-artists` / `#pane-albums` / `#pane-tracks` strip,
 * with the same rule about which of them are on screen at a given width.
 *
 * Hosted twice: as the Library tab itself, and as the pushed uploads listing
 * (`Route.Uploads`), whose [stack] is rooted at `Route.Artists(uploads =
 * true)`. [onOpenUploads] belongs to the first, [onBack] to the second —
 * the tab has no back, and the uploads listing has no deeper uploads.
 *
 * [onFetchUrl] leaves the tab entirely: the fetch panel is a form that wants
 * the whole window, so it stays a destination of the shell's own host rather
 * than becoming a fourth level here.
 */
@Composable
fun LibraryTab(
	stack: PaneStack,
	onFetchUrl: () -> Unit,
	onOpenUploads: (() -> Unit)? = null,
	onBack: (() -> Unit)? = null,
) {
	PaneBackHandler(stack)

	PaneStrip(
		stack = stack,
		titles = listOf(if (onBack != null) "Uploads" else "Artists", "Albums", "Tracks"),
		waiting = { level ->
			PaneWaiting(
				if (level == 1) "Choose an artist to see their albums"
				else "Choose an album to see its tracks"
			)
		},
	) { route ->
		PaneHost(route) {
			composable<Route.Artists> {
				ArtistsScreen(
					onOpenArtist = { refs, name, section ->
						stack.show(
							1,
							Route.Albums(
								artistRefs = ItemRef.encodeAll(refs),
								artistName = name,
								fromUploads = section == LibrarySection.UPLOADS,
								fromCategories = section == LibrarySection.CATEGORIES,
							),
						)
					},
					onFetchUrl = onFetchUrl,
					onOpenUploads = onOpenUploads,
					onBack = onBack,
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
					// Not one level up: a promoted album takes its uploads artist
					// folder with it when it was the only album there, so the level
					// in between may name nothing at all. Back to the artist list,
					// which LibraryRevision has already had re-read.
					onPromoted = { stack.reset() },
				)
			}
		}
	}
}
