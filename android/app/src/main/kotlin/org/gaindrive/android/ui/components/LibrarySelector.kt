package org.gaindrive.android.ui.components

import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.filled.CloudOff
import androidx.compose.material.icons.filled.Dns
import androidx.compose.material3.Checkbox
import androidx.compose.material3.DropdownMenu
import androidx.compose.material3.DropdownMenuItem
import androidx.compose.material3.HorizontalDivider
import androidx.compose.material3.Icon
import androidx.compose.material3.IconButton
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.RadioButton
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import org.gaindrive.android.data.model.BrowseScope
import org.gaindrive.android.data.model.ServerConfig
import org.gaindrive.android.data.model.ServerId

/**
 * Picks what the library is showing: everything at once, one named server, or
 * only what is stored on the device.
 *
 * Offline mode lives here rather than in Settings because it answers the same
 * question the server scope does — "what am I looking at" — and because it is
 * flipped situationally, before a flight or on expensive data, not configured
 * once. Settings has the same switch for completeness; both read the one stored
 * value, so they cannot disagree.
 *
 * The server section is hidden with fewer than two servers configured: the
 * common case should not pay for the general one, and a picker with a single
 * entry is a control that does nothing. The offline row is always there.
 */
@Composable
fun LibrarySelector(
	servers: List<ServerConfig>,
	scope: BrowseScope,
	offline: Boolean,
	onSelectAll: () -> Unit,
	onSelect: (ServerId) -> Unit,
	onSetOffline: (Boolean) -> Unit,
) {
	var open by remember { mutableStateOf(false) }
	val chosen = (scope as? BrowseScope.OneServer)?.id
	val showServers = servers.size >= 2

	Box {
		IconButton(onClick = { open = true }) {
			Icon(
				// The icon carries the state, so being offline is visible
				// without opening anything.
				imageVector = if (offline) Icons.Default.CloudOff else Icons.Default.Dns,
				contentDescription = when {
					offline -> "Showing stored music only"
					else -> "Showing: " +
						(servers.firstOrNull { it.id == chosen }?.name ?: "all servers")
				},
			)
		}
		DropdownMenu(expanded = open, onDismissRequest = { open = false }) {
			if (showServers) {
				DropdownMenuItem(
					leadingIcon = { RadioButton(selected = chosen == null, onClick = null) },
					text = { Text("All servers") },
					onClick = {
						open = false
						onSelectAll()
					},
				)
				servers.forEach { server ->
					DropdownMenuItem(
						leadingIcon = {
							RadioButton(selected = server.id == chosen, onClick = null)
						},
						text = { Text(server.name) },
						onClick = {
							open = false
							onSelect(server.id)
						},
					)
				}
				HorizontalDivider()
			}

			DropdownMenuItem(
				leadingIcon = { Checkbox(checked = offline, onCheckedChange = null) },
				text = {
					Column {
						Text("Offline")
						Text(
							text = "Stored music only",
							style = MaterialTheme.typography.bodySmall,
							color = MaterialTheme.colorScheme.onSurfaceVariant,
						)
					}
				},
				onClick = {
					open = false
					onSetOffline(!offline)
				},
			)
		}
	}
}
