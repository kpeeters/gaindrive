package org.gaindrive.android.ui

import android.widget.Toast
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
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.saveable.rememberSaveable
import androidx.compose.runtime.setValue
import androidx.compose.ui.Modifier
import androidx.compose.ui.platform.LocalContext
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
import org.gaindrive.android.data.model.ItemRef
import org.gaindrive.android.data.model.ServerId
import org.gaindrive.android.ui.browse.AlbumDetailScreen
import org.gaindrive.android.ui.browse.AlbumsScreen
import org.gaindrive.android.ui.browse.ArtistsScreen
import org.gaindrive.android.ui.components.OfflineNote
import org.gaindrive.android.ui.player.CastDeviceSheet
import org.gaindrive.android.ui.player.CastViewModel
import org.gaindrive.android.ui.player.MiniPlayer
import org.gaindrive.android.ui.player.NowPlayingSheet
import org.gaindrive.android.ui.player.PlayerViewModel
import org.gaindrive.android.ui.player.VideoScreen
import org.gaindrive.android.ui.playlists.PlaylistDetailScreen
import org.gaindrive.android.ui.playlists.PlaylistsScreen
import org.gaindrive.android.ui.recents.RecentsScreen
import org.gaindrive.android.ui.search.SearchScreen
import org.gaindrive.android.ui.settings.AppearanceSettingsScreen
import org.gaindrive.android.ui.settings.LibrarySettingsScreen
import org.gaindrive.android.ui.settings.ServerEditScreen
import org.gaindrive.android.ui.settings.ServersSettingsScreen
import org.gaindrive.android.ui.settings.SettingsScreen
import org.gaindrive.android.ui.settings.SettingsViewModel
import org.gaindrive.android.ui.settings.StorageSettingsScreen

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
		// Straight to Servers, not the Settings list: with nothing configured
		// there is exactly one useful thing to do and this is where its button
		// lives.
		if (settings.servers.isEmpty()) Route.SettingsServers else Route.Artists
	}

	val playerViewModel: PlayerViewModel = hiltViewModel()
	val playerState by playerViewModel.state.collectAsStateWithLifecycle()
	var nowPlayingOpen by remember { mutableStateOf(false) }

	// Held here rather than inside the picker so the Now Playing sheet can show
	// whether a device is connected without opening anything. Same view model
	// instance the picker resolves, both being activity-scoped.
	val castViewModel: CastViewModel = hiltViewModel()
	val castDevice by castViewModel.connected.collectAsStateWithLifecycle()
	var castPickerOpen by remember { mutableStateOf(false) }

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
	// form that the user is expected to finish or cancel, nor over a picture
	// that wants the whole screen.
	val showBottomBar = destination?.hasRoute(Route.ServerEdit::class) != true &&
		destination?.hasRoute(Route.Video::class) != true

	// A refusal — today only "video cannot be cast" — outlives the sheet or row
	// the tap came from, so it is shown from the shell rather than from there.
	val playerMessage by playerViewModel.message.collectAsStateWithLifecycle()
	val context = LocalContext.current
	LaunchedEffect(playerMessage) {
		playerMessage?.let {
			Toast.makeText(context, it, Toast.LENGTH_LONG).show()
			playerViewModel.consumeMessage()
		}
	}

	// Sends the user to the picture when what starts playing is a video.
	//
	// Keyed on the item rather than done at the tap, so it covers a video
	// reached by the queue advancing as well as one reached from a list, and so
	// it lives in one place instead of in every browse screen's row handler.
	// Because it keys on the item, backing out to carry on browsing while the
	// sound plays does not bounce the user straight back in.
	val currentRef = playerState.current?.ref
	LaunchedEffect(currentRef, playerState.isVideo) {
		if (currentRef == null || !playerState.isVideo) return@LaunchedEffect
		if (destination?.hasRoute(Route.Video::class) == true) return@LaunchedEffect
		nowPlayingOpen = false
		navController.navigate(Route.Video)
	}

	Scaffold(
		bottomBar = {
			if (showBottomBar) {
				Column {
					// Sits with the player rather than in each screen's app bar:
					// having no network is a fact about the whole app, and one
					// banner is better than five that have to agree.
					OfflineNote(
						online = LocalAvailability.current.online,
						byChoice = LocalAvailability.current.offlineByChoice,
					)
					// Above the navigation bar, and outside the NavHost, so it
					// persists across navigation the way the web client's fixed
					// footer does.
					MiniPlayer(
						state = playerState,
						// A film's bar leads back to the film. Opening the
						// audio-shaped sheet instead would make the user find
						// the way back to the picture from inside it.
						onExpand = {
							if (playerState.isVideo) {
								navController.navigate(Route.Video)
							} else {
								nowPlayingOpen = true
							}
						},
						onTogglePlay = playerViewModel::togglePlayPause,
						onNext = playerViewModel::next,
					)
					NavigationBar {
						TopLevel.entries.forEach { item ->
							NavigationBarItem(
								selected = item == selectedTab,
								onClick = {
									// Tapping the tab you are already in sheds
									// its drill-down. Without it, a tab that
									// restored to an album screen has no way
									// back to its own list except walking the
									// back gesture up the hierarchy.
									if (item == selectedTab) {
										navController.popToTabRoot()
									} else {
										selectedTab = item
										navController.switchTo(item.route)
									}
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
					onOpenArtist = { refs, name ->
						navController.navigate(Route.Albums(ItemRef.encodeAll(refs), name))
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

			composable<Route.Playlists> {
				PlaylistsScreen(
					onOpenPlaylist = { ref, name ->
						navController.navigate(Route.Playlist(ref.encode(), name))
					},
				)
			}

			composable<Route.Playlist> {
				PlaylistDetailScreen(onBack = { navController.popBackStack() })
			}

			composable<Route.Recents> {
				RecentsScreen(
					onOpenAlbum = { ref, title ->
						navController.navigate(Route.Album(ref.encode(), title))
					},
				)
			}

			composable<Route.Search> {
				SearchScreen(
					onOpenArtist = { refs, name ->
						navController.navigate(Route.Albums(ItemRef.encodeAll(refs), name))
					},
					onOpenAlbum = { ref, title ->
						navController.navigate(Route.Album(ref.encode(), title))
					},
				)
			}

			composable<Route.Settings> {
				SettingsScreen(
					onOpenServers = { navController.navigate(Route.SettingsServers) },
					onOpenLibrary = { navController.navigate(Route.SettingsLibrary) },
					onOpenStorage = { navController.navigate(Route.SettingsStorage) },
					onOpenAppearance = { navController.navigate(Route.SettingsAppearance) },
				)
			}

			composable<Route.SettingsServers> {
				ServersSettingsScreen(
					onBack = { navController.popBackStack() },
					onEditServer = { id: ServerId? ->
						navController.navigate(Route.ServerEdit(id?.value))
					},
				)
			}

			composable<Route.SettingsLibrary> {
				LibrarySettingsScreen(onBack = { navController.popBackStack() })
			}

			composable<Route.SettingsStorage> {
				StorageSettingsScreen(onBack = { navController.popBackStack() })
			}

			composable<Route.SettingsAppearance> {
				AppearanceSettingsScreen(onBack = { navController.popBackStack() })
			}

			composable<Route.ServerEdit> {
				ServerEditScreen(onDone = { navController.popBackStack() })
			}

			composable<Route.Video> {
				VideoScreen(onBack = { navController.popBackStack() })
			}
		}
	}

	if (nowPlayingOpen && playerState.isActive) {
		NowPlayingSheet(
			state = playerState,
			casting = castDevice != null,
			onDismiss = { nowPlayingOpen = false },
			onOpenAlbum = { ref, title ->
				// Closed first: the sheet sits on top of the screen it is
				// sending the user to.
				nowPlayingOpen = false
				// Pushed onto whichever tab's stack is current, so Back returns
				// to the search results the track was found in. selectedTab is
				// deliberately left alone for the same reason.
				navController.navigate(Route.Album(ref.encode(), title))
			},
			onTogglePlay = playerViewModel::togglePlayPause,
			onNext = playerViewModel::next,
			onPrevious = playerViewModel::previous,
			onSeek = playerViewModel::seekTo,
			onJumpTo = playerViewModel::jumpTo,
			onCast = { castPickerOpen = true },
			onRemoveFromQueue = playerViewModel::removeFromQueue,
			onWatch = {
				nowPlayingOpen = false
				navController.navigate(Route.Video)
			},
		)
	}

	// Outside the Now Playing sheet's own condition: picking a device must stay
	// possible once that sheet has closed itself behind the tap.
	if (castPickerOpen) {
		CastDeviceSheet(onDismiss = { castPickerOpen = false })
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
			hasRoute(Route.Playlist::class) ||
			hasRoute(Route.ServerEdit::class) ||
			// Listed so back and the tap-the-current-tab gesture shed it like
			// any other drill-down; leaving it does not stop the film.
			hasRoute(Route.Video::class) ||
			// The settings categories drill down like anything else, and being
			// listed here is also what makes tapping the Settings tab shed them.
			hasRoute(Route.SettingsServers::class) ||
			hasRoute(Route.SettingsLibrary::class) ||
			hasRoute(Route.SettingsStorage::class) ||
			hasRoute(Route.SettingsAppearance::class)
		)

/** Long enough to read as a direction, short enough not to be in the way. */
private const val TRANSITION_MS = 280

/**
 * Sheds every drill-down on the current tab, leaving its own list on screen.
 *
 * Popped one at a time rather than with a `popUpTo` of the tab's root: the
 * graph is flat, so a tab root is an ordinary destination with no id to pop
 * back to that holds for every tab, and [isDetail] is already this file's
 * definition of "reached by drilling down". Both pops land in the same frame,
 * so the user sees one transition, not one per level.
 */
private fun NavHostController.popToTabRoot() {
	while (currentBackStackEntry?.destination.isDetail()) {
		// A pop that fails would otherwise spin here forever.
		if (!popBackStack()) return
	}
}

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
