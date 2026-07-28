package org.gaindrive.android.ui.components

import androidx.compose.foundation.layout.Box
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.filled.Dns
import androidx.compose.material3.DropdownMenu
import androidx.compose.material3.DropdownMenuItem
import androidx.compose.material3.Icon
import androidx.compose.material3.IconButton
import androidx.compose.material3.RadioButton
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import org.gaindrive.android.data.model.ServerConfig
import org.gaindrive.android.data.model.ServerId

/**
 * Picks which server the library is showing.
 *
 * Renders nothing at all with fewer than two servers configured: the common
 * case should not pay for the general one, and a picker with a single entry is
 * just a control that does nothing.
 *
 * Sub-phase 2c adds an "All servers" entry above the list.
 */
@Composable
fun ServerSelector(
	servers: List<ServerConfig>,
	current: ServerConfig?,
	onSelect: (ServerId) -> Unit,
) {
	if (servers.size < 2) return

	var open by remember { mutableStateOf(false) }

	Box {
		IconButton(onClick = { open = true }) {
			Icon(
				imageVector = Icons.Default.Dns,
				contentDescription = "Server: ${current?.name ?: "none"}",
			)
		}
		DropdownMenu(expanded = open, onDismissRequest = { open = false }) {
			servers.forEach { server ->
				DropdownMenuItem(
					leadingIcon = {
						RadioButton(
							selected = server.id == current?.id,
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
