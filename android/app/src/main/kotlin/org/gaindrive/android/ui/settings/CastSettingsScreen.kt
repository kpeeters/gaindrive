package org.gaindrive.android.ui.settings

import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.getValue
import androidx.hilt.navigation.compose.hiltViewModel
import androidx.lifecycle.compose.collectAsStateWithLifecycle

/** What a Chromecast is sent, which is not always what this phone would play. */
@Composable
fun CastSettingsScreen(
	onBack: () -> Unit,
	viewModel: SettingsViewModel = hiltViewModel(),
) {
	val state by viewModel.state.collectAsStateWithLifecycle()

	SettingsScaffold(title = "Casting", onBack = onBack) {
		item {
			SwitchRow(
				title = "Cast at original quality",
				// Says which casts it applies to, because that is the whole
				// shape of the setting and there is no way to see it otherwise.
				subtitle = "Send the file as it is stored when the TV fetches it " +
					"from the server itself. Otherwise casting uses the same " +
					"quality as this phone.",
				checked = state.castOriginal,
				onCheckedChange = viewModel::setCastOriginal,
			)
		}

		item {
			Text(
				// The three ways it will silently not apply. Each is a question
				// the track info dialog can answer after the fact, and this is
				// the only place that answers it beforehand.
				text = "A cast relayed through this phone keeps the streaming " +
					"quality: those bytes cross this phone's connection, which is " +
					"the situation relaying exists for. Your account's bitrate " +
					"limit still applies. And a file in a format the TV cannot " +
					"decode is converted anyway, rather than failing to play.\n\n" +
					"The info button in Now Playing shows what is actually being " +
					"sent, and how.",
				style = MaterialTheme.typography.bodySmall,
				color = MaterialTheme.colorScheme.onSurfaceVariant,
			)
		}
	}
}
