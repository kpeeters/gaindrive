package org.gaindrive.android.ui.components

import androidx.compose.foundation.layout.Box
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.filled.Dns
import androidx.compose.material3.DropdownMenu
import androidx.compose.material3.DropdownMenuItem
import androidx.compose.material3.HorizontalDivider
import androidx.compose.material3.Icon
import androidx.compose.material3.IconButton
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
 * Picks what the library is showing: everything at once, or one named server.
 *
 * Renders nothing at all with fewer than two servers configured: the common
 * case should not pay for the general one, and a picker with a single entry is
 * just a control that does nothing.
 */
@Composable
fun ServerSelector(
	servers: List<ServerConfig>,
	scope: BrowseScope,
	onSelectAll: () -> Unit,
	onSelect: (ServerId) -> Unit,
) {
	if (servers.size < 2) return

	var open by remember { mutableStateOf(false) }
	val chosen = (scope as? BrowseScope.OneServer)?.id

	Box {
		IconButton(onClick = { open = true }) {
			Icon(
				imageVector = Icons.Default.Dns,
				contentDescription = "Showing: " +
					(servers.firstOrNull { it.id == chosen }?.name ?: "all servers"),
			)
		}
		DropdownMenu(expanded = open, onDismissRequest = { open = false }) {
			DropdownMenuItem(
				leadingIcon = { RadioButton(selected = chosen == null, onClick = null) },
				text = { Text("All servers") },
				onClick = {
					open = false
					onSelectAll()
				},
			)
			HorizontalDivider()
			servers.forEach { server ->
				DropdownMenuItem(
					leadingIcon = {
						RadioButton(
							selected = server.id == chosen,
							onClick = null,
						)
					},
					text = { Text(server.name) },
					onClick = {
						open = false
						onSelect(server.id)
					},
				)
			}
		}
	}
}
