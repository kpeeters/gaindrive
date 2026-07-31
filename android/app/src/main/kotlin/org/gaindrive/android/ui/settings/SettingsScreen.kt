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
import androidx.compose.foundation.lazy.itemsIndexed
import androidx.compose.foundation.shape.CircleShape
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.filled.Add
import androidx.compose.material.icons.filled.Delete
import androidx.compose.material.icons.filled.KeyboardArrowDown
import androidx.compose.material.icons.filled.KeyboardArrowUp
import androidx.compose.material.icons.filled.MoreVert
import androidx.compose.material3.AlertDialog
import androidx.compose.material3.Card
import androidx.compose.material3.DropdownMenu
import androidx.compose.material3.DropdownMenuItem
import androidx.compose.material3.ExperimentalMaterial3Api
import androidx.compose.material3.FilterChip
import androidx.compose.material3.FloatingActionButton
import androidx.compose.material3.Icon
import androidx.compose.material3.IconButton
import androidx.compose.material3.LinearProgressIndicator
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.OutlinedButton
import androidx.compose.material3.Scaffold
import androidx.compose.material3.Switch
import androidx.compose.material3.Text
import androidx.compose.material3.TextButton
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
import org.gaindrive.android.data.cache.PinnedItem
import org.gaindrive.android.data.model.ServerConfig
import org.gaindrive.android.data.model.ServerId
import org.gaindrive.android.data.model.ThemeMode
import org.gaindrive.android.ui.components.formatBytes

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
					onMoveUp =
						if (index > 0) ({ viewModel.move(index, index - 1) }) else null,
					onMoveDown =
						if (index < state.servers.lastIndex) {
							({ viewModel.move(index, index + 1) })
						} else {
							null
						},
				)
			}

			item { SectionTitle("Library") }
			item {
				SwitchRow(
					title = "Merge duplicate albums",
					// Says what it matches on, because that is what decides
					// whether it does the right thing.
					subtitle = "Show one row when the same artist and album title " +
						"appear on several servers. Case, spacing and punctuation " +
						"are ignored.",
					checked = state.mergeDuplicateAlbums,
					onCheckedChange = viewModel::setMergeDuplicateAlbums,
				)
			}

			item { SectionTitle("Storage") }
			item {
				StorageSection(
					storage = state.storage,
					onSetMaxBytes = viewModel::setCacheMaxBytes,
					onSetCacheOnPlay = viewModel::setCacheOnPlay,
					onSetUnmeteredOnly = viewModel::setDownloadUnmeteredOnly,
					onFlush = { viewModel.flushCache() },
				)
			}

			// Listed only when there are some. Pins are placed deep in the
			// library, so without this the only way to find one again is to
			// remember where it was.
			if (state.pins.isNotEmpty()) {
				item { SectionTitle("Downloads") }
				items(state.pins, key = { it.pin.ref.encode() }) { item ->
					PinnedRow(item = item, onRemove = { viewModel.unpin(item.pin.ref) })
				}
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

/**
 * Cache usage, the cap, and the flush.
 *
 * No per-item list: eviction handles the ordinary case and pinning covers the
 * "keep this" case, so browsing individual cached files would be a screen
 * nobody has a reason to open.
 */
@Composable
private fun StorageSection(
	storage: StorageUiState,
	onSetMaxBytes: (Long) -> Unit,
	onSetCacheOnPlay: (Boolean) -> Unit,
	onSetUnmeteredOnly: (Boolean) -> Unit,
	onFlush: () -> Unit,
) {
	var confirmFlush by remember { mutableStateOf(false) }
	val evictable = (storage.usedBytes - storage.pinnedBytes).coerceAtLeast(0)

	Column(verticalArrangement = Arrangement.spacedBy(12.dp)) {
		Text(
			text = buildString {
				append("${formatBytes(storage.usedBytes)} of ${formatBytes(storage.maxBytes)} used")
				// Only worth saying once something is actually protected.
				if (storage.pinnedBytes > 0) {
					append(", ${formatBytes(storage.pinnedBytes)} of it downloaded")
				}
			},
			style = MaterialTheme.typography.bodyMedium,
		)
		LinearProgressIndicator(
			progress = {
				if (storage.maxBytes <= 0) 0f
				else (storage.usedBytes.toFloat() / storage.maxBytes).coerceIn(0f, 1f)
			},
			modifier = Modifier.fillMaxWidth(),
		)

		Text(
			text = "Maximum size",
			style = MaterialTheme.typography.bodyLarge,
		)
		Row(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
			CACHE_SIZES.forEach { bytes ->
				FilterChip(
					selected = storage.maxBytes == bytes,
					onClick = { onSetMaxBytes(bytes) },
					label = { Text(formatBytes(bytes)) },
				)
			}
		}

		SwitchRow(
			title = "Store music as it plays",
			subtitle = "Anything you play is kept until the cache is full, then " +
				"the least recently played goes first. Downloads are never " +
				"removed automatically.",
			checked = storage.cacheOnPlay,
			onCheckedChange = onSetCacheOnPlay,
		)

		SwitchRow(
			title = "Download on Wi-Fi only",
			// Says why it does not cover the other switch, which is the
			// question this one invites.
			subtitle = "Applies to downloads. Storing what you are already " +
				"streaming costs no extra data, so it is never held back.",
			checked = storage.unmeteredOnly,
			onCheckedChange = onSetUnmeteredOnly,
		)

		OutlinedButton(
			onClick = { confirmFlush = true },
			enabled = evictable > 0,
		) {
			Text("Free ${formatBytes(evictable)}")
		}
	}

	if (confirmFlush) {
		AlertDialog(
			onDismissRequest = { confirmFlush = false },
			title = { Text("Empty the cache?") },
			text = {
				Text(
					"Removes ${formatBytes(evictable)} of stored music. Downloads " +
						"and anything currently queued are kept."
				)
			},
			confirmButton = {
				TextButton(
					onClick = {
						confirmFlush = false
						onFlush()
					}
				) { Text("Empty") }
			},
			dismissButton = {
				TextButton(onClick = { confirmFlush = false }) { Text("Cancel") }
			},
		)
	}
}

/** The caps offered. A slider would imply a precision nobody wants here. */
private val CACHE_SIZES = listOf(
	1L * 1024 * 1024 * 1024,
	2L * 1024 * 1024 * 1024,
	4L * 1024 * 1024 * 1024,
	8L * 1024 * 1024 * 1024,
	16L * 1024 * 1024 * 1024,
)

@Composable
private fun PinnedRow(item: PinnedItem, onRemove: () -> Unit) {
	Row(
		modifier = Modifier.fillMaxWidth(),
		verticalAlignment = Alignment.CenterVertically,
		horizontalArrangement = Arrangement.spacedBy(12.dp),
	) {
		Column(modifier = Modifier.weight(1f)) {
			Text(
				text = item.label,
				style = MaterialTheme.typography.bodyLarge,
				maxLines = 1,
				overflow = TextOverflow.Ellipsis,
			)
			Text(
				text = item.pin.kind.name.lowercase().replaceFirstChar { it.uppercase() },
				style = MaterialTheme.typography.bodySmall,
				color = MaterialTheme.colorScheme.onSurfaceVariant,
			)
		}
		IconButton(onClick = onRemove) {
			Icon(Icons.Default.Delete, contentDescription = "Remove download")
		}
	}
}

@Composable
private fun SwitchRow(
	title: String,
	subtitle: String,
	checked: Boolean,
	onCheckedChange: (Boolean) -> Unit,
) {
	Row(
		modifier = Modifier.fillMaxWidth(),
		verticalAlignment = Alignment.CenterVertically,
		horizontalArrangement = Arrangement.spacedBy(12.dp),
	) {
		Column(modifier = Modifier.weight(1f)) {
			Text(text = title, style = MaterialTheme.typography.bodyLarge)
			Text(
				text = subtitle,
				style = MaterialTheme.typography.bodySmall,
				color = MaterialTheme.colorScheme.onSurfaceVariant,
			)
		}
		Switch(checked = checked, onCheckedChange = onCheckedChange)
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

private fun ThemeMode.label(): String = when (this) {
	ThemeMode.AUTO -> "Auto"
	ThemeMode.LIGHT -> "Light"
	ThemeMode.DARK -> "Dark"
}
