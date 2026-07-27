package org.gaindrive.android.ui

import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.padding
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.filled.Settings
import androidx.compose.material3.ExperimentalMaterial3Api
import androidx.compose.material3.Icon
import androidx.compose.material3.IconButton
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Scaffold
import androidx.compose.material3.Text
import androidx.compose.material3.TopAppBar
import androidx.compose.runtime.Composable
import androidx.compose.runtime.getValue
import androidx.compose.runtime.remember
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.hilt.navigation.compose.hiltViewModel
import androidx.lifecycle.compose.collectAsStateWithLifecycle
import androidx.navigation.compose.NavHost
import androidx.navigation.compose.composable
import androidx.navigation.compose.rememberNavController
import org.gaindrive.android.ui.settings.ServerEditScreen
import org.gaindrive.android.ui.settings.SettingsScreen
import org.gaindrive.android.ui.settings.SettingsViewModel

@Composable
fun GainDriveApp(settingsViewModel: SettingsViewModel = hiltViewModel()) {
	val settings by settingsViewModel.state.collectAsStateWithLifecycle()
	val navController = rememberNavController()

	// Hold the first frame until the server list has actually loaded.
	// Rendering the library and then jumping to Settings would look like a
	// glitch, and rendering Settings and jumping away would look worse.
	if (!settings.loaded) {
		Box(modifier = Modifier.fillMaxSize())
		return
	}

	// Decided once, then held. NavHost rebuilds its graph when startDestination
	// changes, so recomputing this would reset the back stack the moment the
	// first server is saved — throwing the user out of Settings just as they
	// finish adding it.
	val startDestination: Route = remember {
		if (settings.servers.isEmpty()) Route.Settings else Route.Library
	}

	NavHost(
		navController = navController,
		// No servers configured means there is nothing to browse, so Settings
		// is where the app starts rather than somewhere we bounce to.
		startDestination = startDestination,
	) {
		composable<Route.Library> {
			LibraryPlaceholder(onOpenSettings = { navController.navigate(Route.Settings) })
		}

		composable<Route.Settings> {
			SettingsScreen(
				onEditServer = { id -> navController.navigate(Route.ServerEdit(id?.value)) },
			)
		}

		composable<Route.ServerEdit> {
			// The route's serverId reaches ServerEditViewModel through its
			// SavedStateHandle, so nothing needs passing down by hand here.
			ServerEditScreen(onDone = { navController.popBackStack() })
		}
	}
}

/** Phase 2 replaces this with the browsing destinations and bottom navigation. */
@OptIn(ExperimentalMaterial3Api::class)
@Composable
private fun LibraryPlaceholder(onOpenSettings: () -> Unit) {
	Scaffold(
		topBar = {
			TopAppBar(
				title = { Text("GainDrive") },
				actions = {
					IconButton(onClick = onOpenSettings) {
						Icon(Icons.Default.Settings, contentDescription = "Settings")
					}
				},
			)
		},
	) { insets ->
		Box(
			modifier = Modifier.fillMaxSize().padding(insets),
			contentAlignment = Alignment.Center,
		) {
			Text("Library — Phase 2", style = MaterialTheme.typography.headlineSmall)
		}
	}
}
