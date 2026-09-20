package org.gaindrive.android.ui.settings

import androidx.compose.runtime.Composable
import androidx.compose.runtime.getValue
import androidx.hilt.lifecycle.viewmodel.compose.hiltViewModel
import androidx.lifecycle.compose.collectAsStateWithLifecycle

/** How the browse screens present what the servers return. */
@Composable
fun LibrarySettingsScreen(
	onBack: (() -> Unit)?,
	viewModel: SettingsViewModel = hiltViewModel(),
) {
	val state by viewModel.state.collectAsStateWithLifecycle()

	SettingsScaffold(title = "Library", onBack = onBack) {
		item {
			SwitchRow(
				title = "Merge duplicate albums",
				// Says what it matches on, because that is what decides whether
				// it does the right thing.
				subtitle = "Show one row when the same artist and album title " +
					"appear on several servers. Case, spacing and punctuation " +
					"are ignored.",
				checked = state.mergeDuplicateAlbums,
				onCheckedChange = viewModel::setMergeDuplicateAlbums,
			)
		}
	}
}
