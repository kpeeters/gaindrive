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
import androidx.compose.material3.Scaffold
import androidx.compose.material3.Text
import androidx.compose.material3.adaptive.ExperimentalMaterial3AdaptiveApi
import androidx.compose.material3.adaptive.currentWindowAdaptiveInfo
import androidx.compose.material3.adaptive.navigationsuite.NavigationSuiteScaffold
import androidx.compose.material3.adaptive.navigationsuite.NavigationSuiteScaffoldDefaults
import androidx.compose.material3.adaptive.navigationsuite.NavigationSuiteType
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
import kotlinx.coroutines.flow.StateFlow
import org.gaindrive.android.data.model.ServerId
import org.gaindrive.android.playback.cast.CastDeviceKind
import org.gaindrive.android.playback.cast.kind
import org.gaindrive.android.ui.adaptive.rememberPaneStack
import org.gaindrive.android.ui.browse.AlbumDetailScreen
import org.gaindrive.android.ui.components.OfflineNote
import org.gaindrive.android.ui.fetch.FetchUrlScreen
import org.gaindrive.android.ui.player.CastDeviceSheet
import org.gaindrive.android.ui.player.CastViewModel
import org.gaindrive.android.ui.player.MiniPlayer
import org.gaindrive.android.ui.player.NowPlayingSheet
import org.gaindrive.android.ui.player.PlayerViewModel
import org.gaindrive.android.ui.player.TrackInfoDialog
import org.gaindrive.android.ui.player.VideoScreen
import org.gaindrive.android.ui.player.WiiMControlsSheet
import org.gaindrive.android.ui.settings.ServerEditScreen
import org.gaindrive.android.ui.settings.SettingsViewModel
import org.gaindrive.android.ui.tabs.LibraryTab
import org.gaindrive.android.ui.tabs.PlaylistsTab
import org.gaindrive.android.ui.tabs.RecentsTab
import org.gaindrive.android.ui.tabs.SearchTab
import org.gaindrive.android.ui.tabs.SettingsTab

/**
 * [sharedUrl] carries a URL another app sent us, and [onSharedUrlHandled] says
 * it has been acted on. They are parameters rather than another view model
 * because the value comes from an `Intent`, which only the activity sees.
 */
// currentWindowAdaptiveInfo() is the only experimental thing here;
// NavigationSuiteScaffold itself is stable at material3 1.3.1.
@OptIn(ExperimentalMaterial3AdaptiveApi::class)
@Composable
fun GainDriveApp(
	settingsViewModel: SettingsViewModel = hiltViewModel(),
	sharedUrl: StateFlow<String?>,
	onSharedUrlHandled: () -> Unit,
) {
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
		// Straight to Settings, whose own path starts at Servers when nothing
		// is configured — see `stacks` below. There is exactly one useful
		// thing to do on a first run and that is where its button lives.
		if (settings.servers.isEmpty()) Route.Settings else Route.Artists
	}

	// One path per tab — see PaneStack. Held here rather than inside each tab
	// for two reasons: a tab keeps its drill-down while the user is away from
	// it without depending on the navigation library's saveState/restoreState
	// bookkeeping, and tapping the tab you are already in can shed that
	// drill-down, which the tab itself has no way to reach.
	val stacks = mapOf(
		TopLevel.ARTISTS to rememberPaneStack { listOf(Route.Artists) },
		TopLevel.PLAYLISTS to rememberPaneStack { listOf(Route.Playlists) },
		TopLevel.RECENTS to rememberPaneStack { listOf(Route.Recents) },
		TopLevel.SEARCH to rememberPaneStack { listOf(Route.Search) },
		TopLevel.SETTINGS to rememberPaneStack {
			// First run: with nothing configured there is exactly one useful
			// thing to do, so the tab opens on Servers rather than on the
			// category list. Evaluated once, so a path restored after process
			// death is not overruled by the check firing again.
			if (settings.servers.isEmpty()) {
				listOf(Route.Settings, Route.SettingsServers)
			} else {
				listOf(Route.Settings)
			}
		},
	)

	val playerViewModel: PlayerViewModel = hiltViewModel()
	val playerState by playerViewModel.state.collectAsStateWithLifecycle()
	var nowPlayingOpen by remember { mutableStateOf(false) }
	var trackInfoOpen by remember { mutableStateOf(false) }

	// Held here rather than inside the picker so the Now Playing sheet can show
	// whether a device is connected without opening anything. Same view model
	// instance the picker resolves, both being activity-scoped.
	val castViewModel: CastViewModel = hiltViewModel()
	val castDevice by castViewModel.connected.collectAsStateWithLifecycle()
	var castPickerOpen by remember { mutableStateOf(false) }
	var wiimControlsOpen by remember { mutableStateOf(false) }

	val backStackEntry by navController.currentBackStackEntryAsState()
	val destination = backStackEntry?.destination

	// Which tab is lit. Tracked rather than derived from the current route,
	// because four destinations are not tab roots and belong to no tab: the
	// album the Now Playing sheet opens, the server editor, the fetch panel and
	// the picture. A tab's own drill-down no longer comes into it — that is
	// inside the tab, which is why the effect below can be as narrow as it is.
	var selectedTab by rememberSaveable {
		mutableStateOf(if (settings.servers.isEmpty()) TopLevel.SETTINGS else TopLevel.ARTISTS)
	}

	// ...but it still has to follow the destination whenever the destination is
	// itself a tab's root, because plenty of ways out of a tab do not go through
	// the bar: a back press or back gesture out of Settings lands on the library
	// with nothing to tell the bar it has left. A tab left lit that the user is
	// no longer in makes the next tap on it take the "already here" branch, which
	// sheds the drill-down instead of switching — and leaves them there, since
	// every tap after that does the same nothing.
	//
	// Only tab roots. A detail is deliberately not matched: an album reached from
	// Search has to keep Search lit, which is why this is tracked at all.
	LaunchedEffect(destination) {
		TopLevel.entries
			.firstOrNull { destination?.hasRoute(it.route::class) == true }
			?.let { selectedTab = it }
	}

	// The navigation surface is for switching top-level sections; it has no
	// meaning on a form that the user is expected to finish or cancel, nor over
	// a picture that wants the whole screen. The player bar goes with it, as it
	// always has.
	val showNavAndPlayer = destination?.hasRoute(Route.ServerEdit::class) != true &&
		destination?.hasRoute(Route.FetchUrl::class) != true &&
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

	// Opens the fetch panel on a URL shared with the app.
	//
	// Marked handled before navigating, so the share is acted on exactly once:
	// this effect re-runs on the null that follows and returns immediately. The
	// URL travels in the route from here on, which is what makes the panel
	// survive a rotation without the activity being asked again.
	val pendingShare by sharedUrl.collectAsStateWithLifecycle()
	LaunchedEffect(pendingShare) {
		val url = pendingShare ?: return@LaunchedEffect
		onSharedUrlHandled()
		nowPlayingOpen = false
		navController.navigate(Route.FetchUrl(url))
	}

	// Compact windows get a bottom bar, medium and expanded a navigation rail.
	// That is what Material 3's adaptive navigation guidance asks for, and it
	// lands within 50dp of the web client's own 650px sidebar boundary. The
	// default rule also keeps a *landscape phone* on the bottom bar — it tests
	// compact height as well as compact width — which is right, since a rail
	// there would take width from the one orientation that has least of it.
	//
	// None is how the full-window destinations are spelled. As a layout type
	// rather than as a bar simply not drawn, the suite keeps one description of
	// the shell instead of two — and it is also why ARCHITECTURE.md's "no
	// navigation drawer" rule is untouched by any of this: the rule rejects the
	// drawer, and the suite is never asked for one.
	val layoutType = if (showNavAndPlayer) {
		NavigationSuiteScaffoldDefaults.calculateFromAdaptiveInfo(currentWindowAdaptiveInfo())
	} else {
		NavigationSuiteType.None
	}

	NavigationSuiteScaffold(
		layoutType = layoutType,
		navigationSuiteItems = {
			// `tab`, not `item`: the loop variable would otherwise shadow this
			// scope's own `item` function at every call below.
			TopLevel.entries.forEach { tab ->
				item(
					selected = tab == selectedTab,
					onClick = {
						// Tapping the tab you are already in sheds its
						// drill-down. Without it, a tab that restored to an
						// album screen has no way back to its own list except
						// walking the back gesture up the hierarchy.
						if (tab == selectedTab) {
							stacks.getValue(tab).reset()
							navController.popToTabRoot()
						} else {
							selectedTab = tab
							navController.switchTo(tab.route)
						}
					},
					icon = { Icon(tab.icon, contentDescription = tab.label) },
					label = { Text(tab.label) },
				)
			}
		},
	) {
		Scaffold(
			bottomBar = {
				if (showNavAndPlayer) {
					Column {
						// Sits with the player rather than in each screen's app bar:
						// having no network is a fact about the whole app, and one
						// banner is better than five that have to agree.
						OfflineNote(
							online = LocalAvailability.current.online,
							byChoice = LocalAvailability.current.offlineByChoice,
						)
						// The bottom of the *content* column, not of the window —
						// which at compact width is above the navigation bar, as
						// before, and beside the rail at medium and expanded. Both
						// match the web client, where #player is a grid row that
						// the full-height sidebar sits next to. Outside the NavHost
						// either way, so it persists across navigation.
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
							onPrevious = playerViewModel::previous,
							onSeek = playerViewModel::seekTo,
						)
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
					LibraryTab(
						stack = stacks.getValue(TopLevel.ARTISTS),
						// The same panel a shared URL opens, with nothing in its URL
						// field. A form wants the whole window, so it is pushed onto
						// the shell's own host rather than into a pane.
						onFetchUrl = { navController.navigate(Route.FetchUrl("")) },
					)
				}

				composable<Route.Playlists> {
					PlaylistsTab(stacks.getValue(TopLevel.PLAYLISTS))
				}

				composable<Route.Recents> {
					RecentsTab(stacks.getValue(TopLevel.RECENTS))
				}

				composable<Route.Search> {
					SearchTab(stacks.getValue(TopLevel.SEARCH))
				}

				composable<Route.Settings> {
					SettingsTab(
						stack = stacks.getValue(TopLevel.SETTINGS),
						// A form with a validating action, so it takes the window
						// rather than a pane — see SettingsTab.
						onEditServer = { id: ServerId? ->
							navController.navigate(Route.ServerEdit(id?.value))
						},
					)
				}

				// The one album that is not a pane of some tab. It is reached only
				// from the Now Playing sheet, where there is no list beside it to
				// go back to and no tab whose strip it belongs in — the sheet
				// covers whatever the user was doing, and Back should return them
				// to exactly that. Leaving this path on the shell's own host is
				// what keeps that true, and unchanged.
				composable<Route.Album> {
					AlbumDetailScreen(
						onBack = { navController.popBackStack() },
						onPromoted = { navController.popBackStack() },
					)
				}

				composable<Route.ServerEdit> {
					ServerEditScreen(onDone = { navController.popBackStack() })
				}

				// Always navigated *onto* the start destination, never the start
				// destination itself, so there is something to pop back to even when
				// a share is what launched the app.
				composable<Route.FetchUrl> {
					FetchUrlScreen(onDone = { navController.popBackStack() })
				}

				composable<Route.Video> {
					VideoScreen(
						onBack = { navController.popBackStack() },
						onCast = { castPickerOpen = true },
						onInfo = { trackInfoOpen = true },
					)
				}
			}
		}
	}

	if (nowPlayingOpen && playerState.isActive) {
		NowPlayingSheet(
			state = playerState,
			casting = castDevice != null,
			wiim = castDevice?.kind == CastDeviceKind.WIIM,
			onDismiss = { nowPlayingOpen = false },
			onOpenAlbum = { ref, title ->
				// Closed first: the sheet sits on top of the screen it is
				// sending the user to.
				nowPlayingOpen = false
				// The shell's own album, above whichever tab is current, so
				// Back returns to what the sheet was covering. selectedTab is
				// deliberately left alone for the same reason.
				navController.navigate(Route.Album(ref.encode(), title))
			},
			onTogglePlay = playerViewModel::togglePlayPause,
			onNext = playerViewModel::next,
			onPrevious = playerViewModel::previous,
			onSeek = playerViewModel::seekTo,
			onJumpTo = playerViewModel::jumpTo,
			onCast = { castPickerOpen = true },
			onWiiM = { wiimControlsOpen = true },
			onInfo = { trackInfoOpen = true },
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

	// Outside it for the same reason, and gated on the device still being a WiiM:
	// the sheet's whole content is that device's own API, and casting can be
	// stopped from the picker while this is open.
	if (wiimControlsOpen && castDevice?.kind == CastDeviceKind.WIIM) {
		WiiMControlsSheet(onDismiss = { wiimControlsOpen = false })
	}

	// Outside it for the same reason, and gated on there being a track: the
	// dialog describes one, and the queue can empty underneath it.
	if (trackInfoOpen) {
		playerState.current?.let { current ->
			TrackInfoDialog(
				current = current,
				casting = castDevice != null,
				onDismiss = { trackInfoOpen = false },
			)
		}
	}
}

/**
 * Destinations reached by drilling down rather than by picking a tab. Only
 * these get the sliding push; a tab switch is a lateral move.
 */
private fun NavDestination?.isDetail(): Boolean =
	this != null && (
		// Reached only from the Now Playing sheet; every other album is a pane.
		hasRoute(Route.Album::class) ||
			hasRoute(Route.ServerEdit::class) ||
			// Reached from outside the app entirely, but shed by back and by
			// the tap-the-current-tab gesture like any other drill-down.
			hasRoute(Route.FetchUrl::class) ||
			// Listed so back and the tap-the-current-tab gesture shed it like
			// any other drill-down; leaving it does not stop the film.
			hasRoute(Route.Video::class)
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
 *
 * Only the four destinations that are not panes of a tab — the album the Now
 * Playing sheet opens, the server editor, the fetch panel and the picture.
 * A tab's own drill-down is its own `PaneStack`'s, and the caller resets that
 * beside this call.
 *
 * The [previousBackStackEntry] test is belt and braces: popping the only entry
 * would leave the NavHost with nothing to draw, which is a blank screen no
 * gesture recovers from, and every start destination is now a tab root.
 */
private fun NavHostController.popToTabRoot() {
	while (currentBackStackEntry?.destination.isDetail() && previousBackStackEntry != null) {
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
