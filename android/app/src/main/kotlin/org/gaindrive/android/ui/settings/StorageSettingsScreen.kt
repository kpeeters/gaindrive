package org.gaindrive.android.ui.settings

import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.lazy.items
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.filled.Delete
import androidx.compose.material3.AlertDialog
import androidx.compose.material3.FilterChip
import androidx.compose.material3.Icon
import androidx.compose.material3.IconButton
import androidx.compose.material3.LinearProgressIndicator
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.OutlinedButton
import androidx.compose.material3.Text
import androidx.compose.material3.TextButton
import androidx.compose.runtime.Composable
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.dp
import androidx.hilt.navigation.compose.hiltViewModel
import androidx.lifecycle.compose.collectAsStateWithLifecycle
import org.gaindrive.android.data.cache.PinPhase
import org.gaindrive.android.data.cache.PinStatus
import org.gaindrive.android.data.cache.PinnedItem
import org.gaindrive.android.ui.components.formatBytes

/**
 * What is stored on the device, and whether to use the network at all.
 *
 * Offline mode is here as well as in the app bar's library picker. The picker
 * is where it gets flipped in practice; this is where someone looks when they
 * are wondering why nothing is loading. Both read the one stored value.
 */
@Composable
fun StorageSettingsScreen(
	onBack: () -> Unit,
	viewModel: SettingsViewModel = hiltViewModel(),
) {
	val state by viewModel.state.collectAsStateWithLifecycle()
	val pinStatuses by viewModel.pinStatuses.collectAsStateWithLifecycle()
	val storage = state.storage
	var confirmFlush by remember { mutableStateOf(false) }
	// The item awaiting confirmation, not a boolean: the dialog names what it is
	// about to delete, and the row it came from may scroll away underneath it.
	var confirmRemove by remember { mutableStateOf<PinnedItem?>(null) }
	val evictable = (storage.usedBytes - storage.pinnedBytes).coerceAtLeast(0)

	SettingsScaffold(title = "Storage & offline", onBack = onBack) {
		item {
			SwitchRow(
				title = "Offline mode",
				subtitle = "Never contact a server. Browsing shows what has been " +
					"stored, and only stored music plays.",
				checked = storage.offlineMode,
				onCheckedChange = viewModel::setOfflineMode,
			)
		}

		item { SectionTitle("Cache") }

		item {
			Text(
				text = buildString {
					append(
						"${formatBytes(storage.usedBytes)} of " +
							"${formatBytes(storage.maxBytes)} used"
					)
					// Only worth saying once something is actually protected.
					if (storage.pinnedBytes > 0) {
						append(", ${formatBytes(storage.pinnedBytes)} of it downloaded")
					}
				},
				style = MaterialTheme.typography.bodyMedium,
			)
		}
		item {
			LinearProgressIndicator(
				progress = {
					if (storage.maxBytes <= 0) 0f
					else (storage.usedBytes.toFloat() / storage.maxBytes).coerceIn(0f, 1f)
				},
				modifier = Modifier.fillMaxWidth(),
			)
		}

		item { Text(text = "Maximum size", style = MaterialTheme.typography.bodyLarge) }
		item {
			Row(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
				CACHE_SIZES.forEach { bytes ->
					FilterChip(
						selected = storage.maxBytes == bytes,
						onClick = { viewModel.setCacheMaxBytes(bytes) },
						label = { Text(formatBytes(bytes)) },
					)
				}
			}
		}

		item {
			SwitchRow(
				title = "Store music as it plays",
				subtitle = "Anything you play is kept until the cache is full, then " +
					"the least recently played goes first. Downloads are never " +
					"removed automatically.",
				checked = storage.cacheOnPlay,
				onCheckedChange = viewModel::setCacheOnPlay,
			)
		}

		item {
			SwitchRow(
				title = "Download on Wi-Fi only",
				// Says why it does not cover the other switch, which is the
				// question this one invites.
				subtitle = "Applies to downloads. Storing what you are already " +
					"streaming costs no extra data, so it is never held back.",
				checked = storage.unmeteredOnly,
				onCheckedChange = viewModel::setDownloadUnmeteredOnly,
			)
		}

		item {
			OutlinedButton(onClick = { confirmFlush = true }, enabled = evictable > 0) {
				Text("Free ${formatBytes(evictable)}")
			}
		}

		// Listed only when there are some. Pins are placed deep in the library,
		// so without this the only way to find one again is to remember where
		// it was.
		if (state.pins.isNotEmpty()) {
			// "Pinned", not just "Downloads": the cache is full of downloaded
			// music that comes and goes on its own, and the one thing worth
			// knowing about this list is that none of it does.
			item { SectionTitle("Pinned downloads") }
			item {
				Text(
					text = "Kept until you remove them. Everything else in the " +
						"cache makes way for new music when the space runs out.",
					style = MaterialTheme.typography.bodySmall,
					color = MaterialTheme.colorScheme.onSurfaceVariant,
				)
			}
			items(state.pins, key = { it.pin.ref.encode() }) { item ->
				PinnedRow(
					item = item,
					status = pinStatuses[item.pin.ref.encode()],
					onRemove = { confirmRemove = item },
				)
			}
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
						viewModel.flushCache()
					}
				) { Text("Empty") }
			},
			dismissButton = {
				TextButton(onClick = { confirmFlush = false }) { Text("Cancel") }
			},
		)
	}

	confirmRemove?.let { item ->
		AlertDialog(
			onDismissRequest = { confirmRemove = null },
			title = { Text("Remove download?") },
			text = {
				Text(
					// Says both halves: the audio goes, the music does not.
					// Without the second half this reads like deleting the album.
					"“${item.label}” will be deleted from this device. It will " +
						"still play from the server."
				)
			},
			confirmButton = {
				TextButton(
					onClick = {
						viewModel.unpin(item.pin.ref)
						confirmRemove = null
					}
				) { Text("Remove") }
			},
			dismissButton = {
				TextButton(onClick = { confirmRemove = null }) { Text("Cancel") }
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
private fun PinnedRow(item: PinnedItem, status: PinStatus?, onRemove: () -> Unit) {
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
				text = pinSubtitle(item, status),
				style = MaterialTheme.typography.bodySmall,
				color = if (status?.phase == PinPhase.FAILED) {
					MaterialTheme.colorScheme.error
				} else {
					MaterialTheme.colorScheme.onSurfaceVariant
				},
			)
		}
		IconButton(onClick = onRemove) {
			Icon(Icons.Default.Delete, contentDescription = "Remove download")
		}
	}
}

/**
 * Kind alone was enough when a pin was just an intent. Now that a download can
 * be waiting or broken, this is the one screen that can say so in words — and
 * the one place a download you have given up on can be deleted.
 */
private fun pinSubtitle(item: PinnedItem, status: PinStatus?): String {
	val kind = item.pin.kind.name.lowercase().replaceFirstChar { it.uppercase() }
	return when (status?.phase) {
		null, PinPhase.COMPLETE -> kind
		PinPhase.WAITING -> "$kind · waiting for Wi-Fi"
		PinPhase.FAILED -> "$kind · download failed"
		PinPhase.RUNNING -> "$kind · ${status.stored} of ${status.total} downloaded"
	}
}
