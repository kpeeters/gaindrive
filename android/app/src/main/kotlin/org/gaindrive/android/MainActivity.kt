package org.gaindrive.android

import android.os.Bundle
import androidx.activity.ComponentActivity
import androidx.activity.compose.setContent
import androidx.activity.enableEdgeToEdge
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

			GainDriveTheme(mode = state.themeMode) {
				GainDriveApp(settingsViewModel = viewModel)
			}
		}
	}
}
