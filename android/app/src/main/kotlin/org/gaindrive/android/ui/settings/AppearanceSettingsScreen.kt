package org.gaindrive.android.ui.settings

import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Row
import androidx.compose.material3.FilterChip
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.getValue
import androidx.compose.ui.unit.dp
import androidx.hilt.navigation.compose.hiltViewModel
import androidx.lifecycle.compose.collectAsStateWithLifecycle
import org.gaindrive.android.data.model.ThemeMode

@Composable
fun AppearanceSettingsScreen(
	onBack: (() -> Unit)?,
	viewModel: SettingsViewModel = hiltViewModel(),
) {
	val state by viewModel.state.collectAsStateWithLifecycle()

	SettingsScaffold(title = "Appearance", onBack = onBack) {
		item {
			Row(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
				ThemeMode.entries.forEach { mode ->
					FilterChip(
						selected = state.themeMode == mode,
						onClick = { viewModel.setTheme(mode) },
						label = { Text(mode.label()) },
					)
				}
			}
		}
	}
}
