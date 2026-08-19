package org.gaindrive.android.ui.settings

import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.getValue
import androidx.hilt.navigation.compose.hiltViewModel
import androidx.lifecycle.compose.collectAsStateWithLifecycle
import org.gaindrive.android.BuildConfig
import org.gaindrive.android.data.model.ThemeMode
import org.gaindrive.android.ui.components.formatBytes

/**
 * The top of Settings: a short list of categories, each opening a screen of its
 * own.
 *
 * Nested rather than one long list, which is what this was and was already
 * outgrowing. Casting, metadata editing, user administration and upload are all
 * still to come, and a flat screen would become a scroll nobody reads. Nesting
 * is also what Android's own Settings does, so it needs no explaining.
 *
 * Not a navigation drawer: with five top-level destinations the bottom bar is
 * exactly the control Material 3 intends, and a drawer beside it on a phone
 * would be a second navigation surface competing with the first.
 */
@Composable
fun SettingsScreen(
	onOpenServers: () -> Unit,
	onOpenLibrary: () -> Unit,
	onOpenStorage: () -> Unit,
	onOpenAppearance: () -> Unit,
	viewModel: SettingsViewModel = hiltViewModel(),
) {
	val state by viewModel.state.collectAsStateWithLifecycle()

	SettingsScaffold(title = "Settings", onBack = null) {
		item {
			CategoryRow(
				title = "Servers",
				summary = serversSummary(state),
				onClick = onOpenServers,
			)
		}
		item {
			CategoryRow(
				title = "Library",
				summary = if (state.mergeDuplicateAlbums) "Merging duplicate albums"
				else "Showing duplicates separately",
				onClick = onOpenLibrary,
			)
		}
		item {
			CategoryRow(
				title = "Storage & offline",
				summary = storageSummary(state),
				onClick = onOpenStorage,
			)
		}
		item {
			CategoryRow(
				title = "Appearance",
				summary = state.themeMode.label(),
				onClick = onOpenAppearance,
			)
		}

		// Inline rather than a category of its own. A screen holding one line
		// of version text would be a tap that buys nothing.
		item {
			Text(
				text = "GainDrive ${BuildConfig.VERSION_NAME}\n" +
					"A self-hosted, OpenSubsonic-compatible music client.",
				style = MaterialTheme.typography.bodySmall,
				color = MaterialTheme.colorScheme.onSurfaceVariant,
			)
		}
	}
}

private fun serversSummary(state: SettingsUiState): String {
	if (!state.loaded) return ""
	val total = state.servers.size
	if (total == 0) return "None yet"
	val disabled = state.servers.count { !it.enabled }
	val configured = if (total == 1) "1 configured" else "$total configured"
	return if (disabled == 0) configured else "$configured, $disabled disabled"
}

/**
 * Offline mode wins the line when it is on: it changes what every screen in the
 * app will do, which matters more here than how full the cache is.
 */
private fun storageSummary(state: SettingsUiState): String =
	if (state.storage.offlineMode) "Offline mode on"
	else "${formatBytes(state.storage.usedBytes)} of " +
		"${formatBytes(state.storage.maxBytes)} used"

internal fun ThemeMode.label(): String = when (this) {
	ThemeMode.AUTO -> "Auto"
	ThemeMode.LIGHT -> "Light"
	ThemeMode.DARK -> "Dark"
}
