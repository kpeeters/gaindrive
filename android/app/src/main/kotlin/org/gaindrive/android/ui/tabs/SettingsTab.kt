package org.gaindrive.android.ui.tabs

import androidx.compose.runtime.Composable
import androidx.navigation.compose.composable
import org.gaindrive.android.data.model.ServerId
import org.gaindrive.android.ui.Route
import org.gaindrive.android.ui.adaptive.LocalPaneBack
import org.gaindrive.android.ui.adaptive.PaneBackHandler
import org.gaindrive.android.ui.adaptive.PaneHost
import org.gaindrive.android.ui.adaptive.PaneStack
import org.gaindrive.android.ui.adaptive.PaneStrip
import org.gaindrive.android.ui.adaptive.PaneWaiting
import org.gaindrive.android.ui.settings.AppearanceSettingsScreen
import org.gaindrive.android.ui.settings.CastSettingsScreen
import org.gaindrive.android.ui.settings.LibrarySettingsScreen
import org.gaindrive.android.ui.settings.ServersSettingsScreen
import org.gaindrive.android.ui.settings.SettingsScreen
import org.gaindrive.android.ui.settings.StorageSettingsScreen

/**
 * Settings: the category list, and the category being read beside it.
 *
 * This is the shape Android's own Settings has on a tablet, and it needed no
 * new screen — the section was already a list of categories each opening a
 * screen of its own, which is a list and a detail written down as a stack.
 *
 * Two levels, not three. [Route.ServerEdit] would fit a third pane, and is
 * deliberately left as a destination of the shell's own host: it is a form
 * with a validating action, and ARCHITECTURE.md's rule that a navigation
 * surface has no meaning over such a form is worth more than a third pane on
 * the one screen where servers are added.
 */
@Composable
fun SettingsTab(stack: PaneStack, onEditServer: (ServerId?) -> Unit) {
	PaneBackHandler(stack)

	PaneStrip(
		stack = stack,
		titles = listOf("Settings", "Section"),
		waiting = { PaneWaiting("Choose a section") },
	) { route ->
		PaneHost(route) {
			composable<Route.Settings> {
				SettingsScreen(
					onOpenServers = { stack.show(1, Route.SettingsServers) },
					onOpenLibrary = { stack.show(1, Route.SettingsLibrary) },
					onOpenStorage = { stack.show(1, Route.SettingsStorage) },
					onOpenCasting = { stack.show(1, Route.SettingsCasting) },
					onOpenAppearance = { stack.show(1, Route.SettingsAppearance) },
				)
			}
			composable<Route.SettingsServers> {
				ServersSettingsScreen(
					onBack = LocalPaneBack.current,
					onEditServer = onEditServer,
				)
			}
			composable<Route.SettingsLibrary> {
				LibrarySettingsScreen(onBack = LocalPaneBack.current)
			}
			composable<Route.SettingsStorage> {
				StorageSettingsScreen(onBack = LocalPaneBack.current)
			}
			composable<Route.SettingsCasting> {
				CastSettingsScreen(onBack = LocalPaneBack.current)
			}
			composable<Route.SettingsAppearance> {
				AppearanceSettingsScreen(onBack = LocalPaneBack.current)
			}
		}
	}
}
