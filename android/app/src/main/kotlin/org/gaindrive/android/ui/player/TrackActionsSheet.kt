package org.gaindrive.android.ui.player

import androidx.compose.foundation.clickable
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.heightIn
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.lazy.items
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.automirrored.filled.PlaylistAdd
import androidx.compose.material.icons.automirrored.filled.QueueMusic
import androidx.compose.material.icons.filled.Add
import androidx.compose.material.icons.filled.Check
import androidx.compose.material3.CircularProgressIndicator
import androidx.compose.material3.ExperimentalMaterial3Api
import androidx.compose.material3.HorizontalDivider
import androidx.compose.material3.Icon
import androidx.compose.material3.IconButton
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.ModalBottomSheet
import androidx.compose.material3.OutlinedTextField
import androidx.compose.material3.Text
import androidx.compose.material3.TextButton
import androidx.compose.material3.rememberModalBottomSheetState
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.graphics.vector.ImageVector
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.dp
import androidx.hilt.navigation.compose.hiltViewModel
import androidx.lifecycle.compose.collectAsStateWithLifecycle
import org.gaindrive.android.data.model.Song
import org.gaindrive.android.ui.Load

/**
 * What a long press on a track offers. Kept separate from the album screen so
 * the playlist, recents and search listings raise the same sheet.
 */
@OptIn(ExperimentalMaterial3Api::class)
@Composable
fun TrackActionsSheet(
	song: Song,
	onDismiss: () -> Unit,
	onPlayNext: () -> Unit,
	onAddToQueue: () -> Unit,
	playlists: AddToPlaylistViewModel = hiltViewModel(),
) {
	val sheetState = rememberModalBottomSheetState()

	// The picker replaces the actions inside this sheet rather than opening a
	// second one on top: stacked modal sheets fight over the scrim and leave
	// the user two things to dismiss.
	var picking by remember { mutableStateOf(false) }

	val done by playlists.done.collectAsStateWithLifecycle()
	LaunchedEffect(done) {
		if (done) {
			playlists.consumeDone()
			onDismiss()
		}
	}

	ModalBottomSheet(onDismissRequest = onDismiss, sheetState = sheetState) {
		Column(modifier = Modifier.padding(bottom = 24.dp)) {
			Column(modifier = Modifier.padding(horizontal = 24.dp, vertical = 8.dp)) {
				Text(
					text = song.title,
					style = MaterialTheme.typography.titleMedium,
					maxLines = 1,
					overflow = TextOverflow.Ellipsis,
				)
				Text(
					text = listOf(song.artistName, song.albumTitle)
						.filter { it.isNotBlank() }
						.joinToString(" · "),
					style = MaterialTheme.typography.bodySmall,
					color = MaterialTheme.colorScheme.onSurfaceVariant,
					maxLines = 1,
					overflow = TextOverflow.Ellipsis,
				)
			}

			HorizontalDivider(color = MaterialTheme.colorScheme.outlineVariant)

			if (picking) {
				PlaylistPicker(song = song, playlists = playlists)
			} else {
				SheetAction(
					icon = Icons.AutoMirrored.Filled.PlaylistAdd,
					label = "Play next",
				) {
					onPlayNext()
					onDismiss()
				}
				SheetAction(
					icon = Icons.AutoMirrored.Filled.QueueMusic,
					label = "Add to queue",
				) {
					onAddToQueue()
					onDismiss()
				}
				SheetAction(
					icon = Icons.Default.Add,
					label = "Add to playlist",
				) {
					picking = true
				}
			}
		}
	}
}

/**
 * The playlists on the track's own server, plus a field for a new one. Only
 * that server's playlists are offered — one cannot hold a song from elsewhere.
 */
@Composable
private fun PlaylistPicker(song: Song, playlists: AddToPlaylistViewModel) {
	val state by playlists.state.collectAsStateWithLifecycle()
	val busy by playlists.busy.collectAsStateWithLifecycle()
	val error by playlists.error.collectAsStateWithLifecycle()

	LaunchedEffect(song.ref.server) { playlists.load(song.ref.server) }

	var newName by remember { mutableStateOf("") }

	Column {
		error?.let { message ->
			Text(
				text = message,
				style = MaterialTheme.typography.bodySmall,
				color = MaterialTheme.colorScheme.error,
				modifier = Modifier.padding(horizontal = 24.dp, vertical = 8.dp),
			)
		}

		Row(
			modifier = Modifier.padding(start = 24.dp, end = 16.dp, top = 12.dp),
			verticalAlignment = Alignment.CenterVertically,
			horizontalArrangement = Arrangement.spacedBy(8.dp),
		) {
			OutlinedTextField(
				value = newName,
				onValueChange = {
					newName = it
					playlists.clearError()
				},
				label = { Text("New playlist") },
				singleLine = true,
				enabled = !busy,
				modifier = Modifier.weight(1f),
			)
			IconButton(
				onClick = {
					playlists.create(song.ref.server, newName, song.ref.id)
				},
				enabled = !busy && newName.isNotBlank(),
			) {
				Icon(Icons.Default.Check, contentDescription = "Create playlist")
			}
		}

		// Written out rather than handed to LoadStateBox: that fills its box,
		// and a sheet listing two playlists should be two rows tall, not a
		// third of the screen.
		when (val loaded = state) {
			is Load.Loading -> CircularProgressIndicator(
				modifier = Modifier.padding(24.dp).size(20.dp),
				strokeWidth = 2.dp,
			)

			is Load.Failed -> Row(
				modifier = Modifier.padding(start = 24.dp, end = 8.dp),
				verticalAlignment = Alignment.CenterVertically,
			) {
				Text(
					text = loaded.message,
					style = MaterialTheme.typography.bodyMedium,
					color = MaterialTheme.colorScheme.onSurfaceVariant,
					modifier = Modifier.weight(1f),
				)
				TextButton(onClick = playlists::reload) { Text("Try again") }
			}

			is Load.Ready -> if (loaded.value.isEmpty()) {
				Text(
					text = "No playlists on this server yet.",
					style = MaterialTheme.typography.bodyMedium,
					color = MaterialTheme.colorScheme.onSurfaceVariant,
					modifier = Modifier.padding(horizontal = 24.dp, vertical = 16.dp),
				)
			} else {
				// Bounded so a long list cannot push the new-playlist field off
				// the top of the sheet.
				LazyColumn(modifier = Modifier.heightIn(max = 320.dp)) {
					items(loaded.value, key = { it.ref.encode() }) { playlist ->
						SheetRow(
							label = playlist.name,
							enabled = !busy,
							onClick = { playlists.add(playlist.ref, song.ref.id) },
						)
					}
				}
			}
		}
	}
}

@Composable
private fun SheetAction(icon: ImageVector, label: String, onClick: () -> Unit) {
	Row(
		modifier = Modifier
			.fillMaxWidth()
			.clickable(onClick = onClick)
			.padding(horizontal = 24.dp, vertical = 16.dp),
		verticalAlignment = Alignment.CenterVertically,
		horizontalArrangement = Arrangement.spacedBy(16.dp),
	) {
		Icon(icon, contentDescription = null)
		Text(text = label, style = MaterialTheme.typography.bodyLarge)
	}
}

@Composable
private fun SheetRow(label: String, enabled: Boolean, onClick: () -> Unit) {
	Text(
		text = label,
		style = MaterialTheme.typography.bodyLarge,
		maxLines = 1,
		overflow = TextOverflow.Ellipsis,
		modifier = Modifier
			.fillMaxWidth()
			.clickable(enabled = enabled, onClick = onClick)
			.padding(horizontal = 24.dp, vertical = 14.dp),
	)
}
