package org.gaindrive.android

import android.Manifest
import android.os.Build
import android.os.Bundle
import androidx.activity.ComponentActivity
import androidx.activity.compose.rememberLauncherForActivityResult
import androidx.activity.compose.setContent
import androidx.activity.enableEdgeToEdge
import androidx.activity.result.contract.ActivityResultContracts
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.getValue
import androidx.hilt.navigation.compose.hiltViewModel
import androidx.lifecycle.compose.collectAsStateWithLifecycle
import dagger.hilt.android.AndroidEntryPoint
import org.gaindrive.android.ui.GainDriveApp
import org.gaindrive.android.ui.settings.SettingsViewModel
import org.gaindrive.android.ui.theme.GainDriveTheme

@AndroidEntryPoint
class MainActivity : ComponentActivity() {
	override fun onCreate(savedInstanceState: Bundle?) {
		super.onCreate(savedInstanceState)
		enableEdgeToEdge()
		setContent {
			// One view model supplies both the theme and the server list, so
			// the theme cannot lag a change to the setting.
			val viewModel: SettingsViewModel = hiltViewModel()
			val state by viewModel.state.collectAsStateWithLifecycle()

			RequestNotificationPermission()

			GainDriveTheme(mode = state.themeMode) {
				GainDriveApp(settingsViewModel = viewModel)
			}
		}
	}
}

/**
 * Asks for POST_NOTIFICATIONS on API 33+, where the media notification needs it.
 *
 * Denial is not an error: playback works either way, the notification is simply
 * silent. So this asks once and never nags.
 */
@androidx.compose.runtime.Composable
private fun RequestNotificationPermission() {
	if (Build.VERSION.SDK_INT < Build.VERSION_CODES.TIRAMISU) return

	val launcher = rememberLauncherForActivityResult(
		contract = ActivityResultContracts.RequestPermission(),
		onResult = { /* Either way, playback is unaffected. */ },
	)
	LaunchedEffect(Unit) {
		launcher.launch(Manifest.permission.POST_NOTIFICATIONS)
	}
}
