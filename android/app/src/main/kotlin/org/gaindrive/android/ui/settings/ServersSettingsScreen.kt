package org.gaindrive.android.ui.settings

import androidx.compose.foundation.background
import androidx.compose.foundation.clickable
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.lazy.itemsIndexed
import androidx.compose.foundation.shape.CircleShape
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.filled.Add
import androidx.compose.material.icons.filled.KeyboardArrowDown
import androidx.compose.material.icons.filled.KeyboardArrowUp
import androidx.compose.material.icons.filled.MoreVert
import androidx.compose.material3.Card
import androidx.compose.material3.DropdownMenu
import androidx.compose.material3.DropdownMenuItem
import androidx.compose.material3.FloatingActionButton
import androidx.compose.material3.Icon
import androidx.compose.material3.IconButton
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clip
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.dp
import androidx.hilt.navigation.compose.hiltViewModel
import androidx.lifecycle.compose.collectAsStateWithLifecycle
import org.gaindrive.android.data.model.ServerConfig
import org.gaindrive.android.data.model.ServerId

/**
 * The configured servers: add, edit, reorder, enable, remove.
 *
 * This is also where an install with no servers starts, so the add button is
 * already on screen rather than a category tap away.
 */
@Composable
fun ServersSettingsScreen(
	onBack: (() -> Unit)?,
	onEditServer: (ServerId?) -> Unit,
	viewModel: SettingsViewModel = hiltViewModel(),
) {
	val state by viewModel.state.collectAsStateWithLifecycle()

	SettingsScaffold(
		title = "Servers",
		onBack = onBack,
		floatingActionButton = {
			FloatingActionButton(onClick = { onEditServer(null) }) {
				Icon(Icons.Default.Add, contentDescription = "Add server")
			}
		},
	) {
		if (state.loaded && state.servers.isEmpty()) {
			item {
				Text(
					text = "No servers yet. Add one to get started.",
					style = MaterialTheme.typography.bodyMedium,
					color = MaterialTheme.colorScheme.onSurfaceVariant,
				)
			}
		}

		if (state.servers.size > 1) {
			item {
				Text(
					text = "Order sets preference: when the same album is on " +
						"several servers, the one highest here wins.",
					style = MaterialTheme.typography.bodySmall,
					color = MaterialTheme.colorScheme.onSurfaceVariant,
				)
			}
		}

		itemsIndexed(state.servers, key = { _, it -> it.id.value }) { index, server ->
			ServerRow(
				server = server,
				onEdit = { onEditServer(server.id) },
				onToggle = { viewModel.setEnabled(server.id, !server.enabled) },
				onRemove = { viewModel.remove(server.id) },
				onMoveUp = if (index > 0) ({ viewModel.move(index, index - 1) }) else null,
				onMoveDown =
					if (index < state.servers.lastIndex) {
						({ viewModel.move(index, index + 1) })
					} else {
						null
					},
			)
		}
	}
}

@Composable
private fun ServerRow(
	server: ServerConfig,
	onEdit: () -> Unit,
	onToggle: () -> Unit,
	onRemove: () -> Unit,
	/** Null at the ends of the list, where the move has nowhere to go. */
	onMoveUp: (() -> Unit)? = null,
	onMoveDown: (() -> Unit)? = null,
) {
	var menuOpen by remember { mutableStateOf(false) }

	Card(modifier = Modifier.fillMaxWidth()) {
		Row(
			modifier = Modifier
				.fillMaxWidth()
				.clickable(onClick = onEdit)
				.padding(16.dp),
			verticalAlignment = Alignment.CenterVertically,
			horizontalArrangement = Arrangement.spacedBy(12.dp),
		) {
			StatusDot(server)

			Column(modifier = Modifier.weight(1f)) {
				Text(
					text = server.name,
					style = MaterialTheme.typography.titleSmall,
					maxLines = 1,
					overflow = TextOverflow.Ellipsis,
				)
				Text(
					text = subtitleFor(server),
					style = MaterialTheme.typography.bodySmall,
					color = MaterialTheme.colorScheme.onSurfaceVariant,
					maxLines = 1,
					overflow = TextOverflow.Ellipsis,
				)
			}

			Box {
				IconButton(onClick = { menuOpen = true }) {
					Icon(Icons.Default.MoreVert, contentDescription = "More")
				}
				DropdownMenu(expanded = menuOpen, onDismissRequest = { menuOpen = false }) {
					// Move up/down rather than drag-and-drop. SCREENS.md asks
					// for drag; a hand-rolled reorderable LazyColumn is a lot of
					// fiddly surface for a list that is two or three rows long,
					// and these are also the accessible version of the gesture.
					onMoveUp?.let { move ->
						DropdownMenuItem(
							text = { Text("Move up") },
							leadingIcon = {
								Icon(Icons.Default.KeyboardArrowUp, contentDescription = null)
							},
							onClick = {
								menuOpen = false
								move()
							},
						)
					}
					onMoveDown?.let { move ->
						DropdownMenuItem(
							text = { Text("Move down") },
							leadingIcon = {
								Icon(Icons.Default.KeyboardArrowDown, contentDescription = null)
							},
							onClick = {
								menuOpen = false
								move()
							},
						)
					}
					DropdownMenuItem(
						text = { Text(if (server.enabled) "Disable" else "Enable") },
						onClick = {
							menuOpen = false
							onToggle()
						},
					)
					DropdownMenuItem(
						text = { Text("Remove") },
						onClick = {
							menuOpen = false
							onRemove()
						},
					)
				}
			}
		}
	}
}

/**
 * Three states worth telling apart at a glance: disabled, needs its password
 * re-entered (the Keystore key went missing), and normal.
 */
@Composable
private fun StatusDot(server: ServerConfig) {
	val colour = when {
		!server.enabled -> MaterialTheme.colorScheme.outline
		server.password.isBlank() -> MaterialTheme.colorScheme.error
		else -> MaterialTheme.colorScheme.primary
	}
	Box(
		modifier = Modifier
			.size(10.dp)
			.clip(CircleShape)
			.background(colour)
	)
}

private fun subtitleFor(server: ServerConfig): String = when {
	server.password.isBlank() -> "${server.username} · password needs re-entering"
	!server.enabled -> "${server.username} · disabled"
	else -> "${server.username} · ${server.url}"
}
