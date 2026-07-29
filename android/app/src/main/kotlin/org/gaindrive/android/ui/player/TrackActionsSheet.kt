package org.gaindrive.android.ui.player

import androidx.compose.foundation.clickable
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.padding
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.automirrored.filled.PlaylistAdd
import androidx.compose.material.icons.automirrored.filled.QueueMusic
import androidx.compose.material3.ExperimentalMaterial3Api
import androidx.compose.material3.HorizontalDivider
import androidx.compose.material3.Icon
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.ModalBottomSheet
import androidx.compose.material3.Text
import androidx.compose.material3.rememberModalBottomSheetState
import androidx.compose.runtime.Composable
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.graphics.vector.ImageVector
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.dp
import org.gaindrive.android.data.model.Song

/**
 * What a long press on a track offers. Kept separate from the album screen so
 * the playlist and search listings can raise the same sheet in sub-phase 2d.
 */
@OptIn(ExperimentalMaterial3Api::class)
@Composable
fun TrackActionsSheet(
	song: Song,
	onDismiss: () -> Unit,
	onPlayNext: () -> Unit,
	onAddToQueue: () -> Unit,
) {
	val sheetState = rememberModalBottomSheetState()

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
