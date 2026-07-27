package org.gaindrive.android.ui.settings

import androidx.compose.foundation.background
import androidx.compose.foundation.clickable
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.PaddingValues
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.lazy.items
import androidx.compose.foundation.shape.CircleShape
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.filled.Add
import androidx.compose.material.icons.filled.MoreVert
import androidx.compose.material3.Card
import androidx.compose.material3.DropdownMenu
import androidx.compose.material3.DropdownMenuItem
import androidx.compose.material3.ExperimentalMaterial3Api
import androidx.compose.material3.FilterChip
import androidx.compose.material3.FloatingActionButton
import androidx.compose.material3.Icon
import androidx.compose.material3.IconButton
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Scaffold
import androidx.compose.material3.Text
import androidx.compose.material3.TopAppBar
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
import org.gaindrive.android.data.model.ThemeMode

@OptIn(ExperimentalMaterial3Api::class)
@Composable
fun SettingsScreen(
	onEditServer: (ServerId?) -> Unit,
	viewModel: SettingsViewModel = hiltViewModel(),
) {
	val state by viewModel.state.collectAsStateWithLifecycle()

	Scaffold(
		topBar = { TopAppBar(title = { Text("Settings") }) },
		floatingActionButton = {
			FloatingActionButton(onClick = { onEditServer(null) }) {
				Icon(Icons.Default.Add, contentDescription = "Add server")
			}
		},
	) { insets ->
		LazyColumn(
			modifier = Modifier.fillMaxSize().padding(insets),
			contentPadding = PaddingValues(16.dp),
			verticalArrangement = Arrangement.spacedBy(12.dp),
		) {
			item { SectionTitle("Servers") }

			if (state.loaded && state.servers.isEmpty()) {
				item {
					Text(
						text = "No servers yet. Add one to get started.",
						style = MaterialTheme.typography.bodyMedium,
						color = MaterialTheme.colorScheme.onSurfaceVariant,
					)
				}
			}

			items(state.servers, key = { it.id.value }) { server ->
				ServerRow(
					server = server,
					onEdit = { onEditServer(server.id) },
					onToggle = { viewModel.setEnabled(server.id, !server.enabled) },
					onRemove = { viewModel.remove(server.id) },
				)
			}

			item { SectionTitle("Appearance") }
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

			item { SectionTitle("About") }
			item {
				Text(
					text = "GainDrive 0.1\n" +
						"A self-hosted, OpenSubsonic-compatible music client.",
					style = MaterialTheme.typography.bodyMedium,
					color = MaterialTheme.colorScheme.onSurfaceVariant,
				)
			}
		}
	}
}

@Composable
private fun SectionTitle(text: String) {
	Text(
		text = text,
		style = MaterialTheme.typography.titleMedium,
		color = MaterialTheme.colorScheme.primary,
		modifier = Modifier.padding(top = 8.dp),
	)
}

@Composable
private fun ServerRow(
	server: ServerConfig,
	onEdit: () -> Unit,
	onToggle: () -> Unit,
	onRemove: () -> Unit,
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

private fun ThemeMode.label(): String = when (this) {
	ThemeMode.AUTO -> "Auto"
	ThemeMode.LIGHT -> "Light"
	ThemeMode.DARK -> "Dark"
}
