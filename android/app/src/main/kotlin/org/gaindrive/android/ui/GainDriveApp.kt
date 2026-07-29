package org.gaindrive.android.ui

import androidx.compose.animation.core.tween
import androidx.compose.animation.fadeIn
import androidx.compose.animation.fadeOut
import androidx.compose.animation.slideInHorizontally
import androidx.compose.animation.slideOutHorizontally
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.consumeWindowInsets
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.padding
import androidx.compose.material3.Icon
import androidx.compose.material3.NavigationBar
import androidx.compose.material3.NavigationBarItem
import androidx.compose.material3.Scaffold
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.saveable.rememberSaveable
import androidx.compose.runtime.setValue
import androidx.compose.ui.Modifier
import androidx.hilt.navigation.compose.hiltViewModel
import androidx.lifecycle.compose.collectAsStateWithLifecycle
import androidx.navigation.NavDestination
import androidx.navigation.NavDestination.Companion.hasRoute
import androidx.navigation.NavGraph.Companion.findStartDestination
import androidx.navigation.NavHostController
import androidx.navigation.compose.NavHost
import androidx.navigation.compose.composable
import androidx.navigation.compose.currentBackStackEntryAsState
import androidx.navigation.compose.rememberNavController
import org.gaindrive.android.data.model.ServerId
import org.gaindrive.android.ui.browse.AlbumDetailScreen
import org.gaindrive.android.ui.browse.AlbumsScreen
import org.gaindrive.android.ui.browse.ArtistsScreen
import org.gaindrive.android.ui.components.EmptyMessage
import org.gaindrive.android.ui.player.MiniPlayer
import org.gaindrive.android.ui.player.NowPlayingSheet
import org.gaindrive.android.ui.player.PlayerViewModel
import org.gaindrive.android.ui.search.SearchScreen
import org.gaindrive.android.ui.settings.ServerEditScreen
import org.gaindrive.android.ui.settings.SettingsScreen
import org.gaindrive.android.ui.settings.SettingsViewModel

@Composable
fun GainDriveApp(settingsViewModel: SettingsViewModel = hiltViewModel()) {
	val settings by settingsViewModel.state.collectAsStateWithLifecycle()
	val navController = rememberNavController()

	// Hold the first frame until the server list has loaded. Rendering the
	// library and then jumping to Settings would look like a glitch, and the
	// reverse would look worse.
	if (!settings.loaded) {
		Box(modifier = Modifier.fillMaxSize())
		return
	}

	// Decided once, then held: NavHost rebuilds its graph when startDestination
	// changes, which would reset the back stack the moment the first server is
	// saved — throwing the user out of Settings just as they finish adding it.
	val startDestination: Route = remember {
		if (settings.servers.isEmpty()) Route.Settings else Route.Artists
	}

	val playerViewModel: PlayerViewModel = hiltViewModel()
	val playerState by playerViewModel.state.collectAsStateWithLifecycle()
	var nowPlayingOpen by remember { mutableStateOf(false) }

	val backStackEntry by navController.currentBackStackEntryAsState()
	val destination = backStackEntry?.destination

	// Which tab is lit. Tracked rather than derived from the current route,
	// because Albums and Album can be reached from either Artists or Search —
	// matching on route class would jump the highlight to Artists when you open
	// an album from a search result.
	var selectedTab by rememberSaveable {
		mutableStateOf(if (settings.servers.isEmpty()) TopLevel.SETTINGS else TopLevel.ARTISTS)
	}

	// The bar is for switching top-level sections; it has no meaning on a
	// form that the user is expected to finish or cancel.
	val showBottomBar = destination?.hasRoute(Route.ServerEdit::class) != true

	Scaffold(
		bottomBar = {
			if (showBottomBar) {
				Column {
					// Above the navigation bar, and outside the NavHost, so it
					// persists across navigation the way the web client's fixed
					// footer does.
					MiniPlayer(
						state = playerState,
						onExpand = { nowPlayingOpen = true },
						onTogglePlay = playerViewModel::togglePlayPause,
						onNext = playerViewModel::next,
					)
					NavigationBar {
						TopLevel.entries.forEach { item ->
							NavigationBarItem(
								selected = item == selectedTab,
								onClick = {
									selectedTab = item
									navController.switchTo(item.route)
								},
								icon = { Icon(item.icon, contentDescription = item.label) },
								label = { Text(item.label) },
							)
						}
					}
				}
			}
		},
	) { insets ->
		NavHost(
			navController = navController,
			startDestination = startDestination,
			// Drilling in slides left, going back slides right, which makes the
			// Artists → Albums → Album hierarchy visible the way the web
			// client's sliding panes do. Switching tabs is a lateral move, not
			// a descent, so it cross-fades instead.
			// slideInHorizontally rather than slideIntoContainer: the latter
			// derives its distance from the difference in container sizes, which
			// between two full-screen destinations is nearly nothing — hence a
			// slide you can barely see. These offsets are explicit multiples of
			// the screen width.
			enterTransition = {
				if (targetState.destination.isDetail()) {
					slideInHorizontally(tween(TRANSITION_MS)) { width -> width }
				} else {
					fadeIn(tween(TRANSITION_MS))
				}
			},
			exitTransition = {
				if (targetState.destination.isDetail()) {
					// The outgoing screen travels a third of the way. That reads
					// as depth, and moves far fewer pixels than sliding both
					// screens the full width would.
					slideOutHorizontally(tween(TRANSITION_MS)) { width -> -width / 3 }
				} else {
					fadeOut(tween(TRANSITION_MS))
				}
			},
			// On the way back the roles reverse: what matters is whether the
			// screen being left was a detail, not the one being returned to.
			popEnterTransition = {
				if (initialState.destination.isDetail()) {
					slideInHorizontally(tween(TRANSITION_MS)) { width -> -width / 3 }
				} else {
					fadeIn(tween(TRANSITION_MS))
				}
			},
			popExitTransition = {
				if (initialState.destination.isDetail()) {
					slideOutHorizontally(tween(TRANSITION_MS)) { width -> width }
				} else {
					fadeOut(tween(TRANSITION_MS))
				}
			},
			// padding() positions the content; consumeWindowInsets() tells the
			// screens' own Scaffolds and TopAppBars that these insets are
			// already accounted for. Without the second call each screen adds
			// the status bar and navigation bar a second time — a doubled gap
			// under the status bar, and a dead strip above the mini-player.
			modifier = Modifier
				.padding(insets)
				.consumeWindowInsets(insets),
		) {
			composable<Route.Artists> {
				ArtistsScreen(
					onOpenArtist = { ref, name ->
						navController.navigate(Route.Albums(ref.encode(), name))
					},
				)
			}

			composable<Route.Albums> {
				AlbumsScreen(
					onBack = { navController.popBackStack() },
					onOpenAlbum = { ref, title ->
						navController.navigate(Route.Album(ref.encode(), title))
					},
				)
			}

			composable<Route.Album> {
				AlbumDetailScreen(onBack = { navController.popBackStack() })
			}

			// Filled in by sub-phase 2d.
			composable<Route.Playlists> { EmptyMessage("Playlists arrive in 2d.") }
			composable<Route.Recents> { EmptyMessage("Recents arrive in 2d.") }
			composable<Route.Search> {
				SearchScreen(
					onOpenArtist = { ref, name ->
						navController.navigate(Route.Albums(ref.encode(), name))
					},
					onOpenAlbum = { ref, title ->
						navController.navigate(Route.Album(ref.encode(), title))
					},
				)
			}

			composable<Route.Settings> {
				SettingsScreen(
					onEditServer = { id: ServerId? ->
						navController.navigate(Route.ServerEdit(id?.value))
					},
				)
			}

			composable<Route.ServerEdit> {
				ServerEditScreen(onDone = { navController.popBackStack() })
			}
		}
	}

	if (nowPlayingOpen && playerState.isActive) {
		NowPlayingSheet(
			state = playerState,
			onDismiss = { nowPlayingOpen = false },
			onTogglePlay = playerViewModel::togglePlayPause,
			onNext = playerViewModel::next,
			onPrevious = playerViewModel::previous,
			onSeek = playerViewModel::seekTo,
			onJumpTo = playerViewModel::jumpTo,
			onRemoveFromQueue = playerViewModel::removeFromQueue,
		)
	}
}

/**
 * Destinations reached by drilling down rather than by picking a tab. Only
 * these get the sliding push; a tab switch is a lateral move.
 */
private fun NavDestination?.isDetail(): Boolean =
	this != null && (
		hasRoute(Route.Albums::class) ||
			hasRoute(Route.Album::class) ||
			hasRoute(Route.ServerEdit::class)
		)

/** Long enough to read as a direction, short enough not to be in the way. */
private const val TRANSITION_MS = 280

/**
 * Standard tab switch: one entry per tab on the back stack, and each tab's
 * scroll position and state preserved while the user is away from it.
 */
private fun NavHostController.switchTo(route: Route) {
	navigate(route) {
		popUpTo(graph.findStartDestination().id) { saveState = true }
		launchSingleTop = true
		restoreState = true
	}
}
