package org.gaindrive.android.ui.settings

import androidx.compose.foundation.clickable
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Column
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.getValue
import androidx.compose.ui.Modifier
import androidx.compose.ui.platform.LocalUriHandler
import androidx.compose.ui.unit.dp
import androidx.hilt.lifecycle.viewmodel.compose.hiltViewModel
import androidx.lifecycle.compose.collectAsStateWithLifecycle
import org.gaindrive.android.BuildConfig
import org.gaindrive.android.data.model.ThemeMode
import org.gaindrive.android.ui.LocalIsTv
import org.gaindrive.android.ui.components.formatBytes

/**
 * The top of Settings: a short list of categories, each opening a screen of its
 * own.
 *
 * Nested rather than one long list, which is what this was and was already
 * outgrowing. Metadata editing, user administration and upload are all still to
 * come, and a flat screen would become a scroll nobody reads. Nesting is also
 * what Android's own Settings does, so it needs no explaining.
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
	onOpenCasting: () -> Unit,
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
				title = "Casting",
				summary = if (state.castOriginal) "Original quality when direct"
				else "Same quality as this phone",
				onClick = onOpenCasting,
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
			AboutBlock()
		}
	}
}

/**
 * Version, authorship and licence.
 *
 * The GPL asks that an interactive program tell the user it is free software
 * and where the terms are; a settings screen is the only place this app has to
 * say so. The website is the one line that is a control rather than prose, so
 * it is the only one coloured and clickable - a whole paragraph in link blue
 * reads as a mis-styled screen.
 */
@Composable
private fun AboutBlock() {
	val uriHandler = LocalUriHandler.current

	Column(verticalArrangement = Arrangement.spacedBy(4.dp)) {
		Text(
			text = "GainDrive ${BuildConfig.VERSION_NAME}\n" +
				"A self-hosted, OpenSubsonic-compatible music client.\n" +
				"Copyright (C) 2026  Kasper Peeters\n" +
				"Licensed under the GNU General Public License, version 3 " +
				"or later.",
			style = MaterialTheme.typography.bodySmall,
			color = MaterialTheme.colorScheme.onSurfaceVariant,
		)
		// Plain text on TV: many have no browser, and Play's TV review fails
		// an app that tries to launch one (TV-WB). The address still reads.
		val isTv = LocalIsTv.current
		Text(
			text = WEBSITE,
			style = MaterialTheme.typography.bodySmall,
			color = if (isTv) MaterialTheme.colorScheme.onSurfaceVariant
				else MaterialTheme.colorScheme.primary,
			modifier = if (isTv) Modifier
				else Modifier.clickable { uriHandler.openUri(WEBSITE) },
		)
	}
}

private const val WEBSITE = "https://www.gaindrive.org"

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
