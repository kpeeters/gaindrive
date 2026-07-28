package org.gaindrive.android.ui.player

import androidx.compose.foundation.clickable
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.padding
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.filled.Pause
import androidx.compose.material.icons.filled.PlayArrow
import androidx.compose.material.icons.filled.SkipNext
import androidx.compose.material3.Icon
import androidx.compose.material3.IconButton
import androidx.compose.material3.LinearProgressIndicator
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Surface
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.dp
import org.gaindrive.android.playback.PlayerState
import org.gaindrive.android.ui.components.CoverThumb

/**
 * The persistent bar above the bottom navigation. Sits in the app shell rather
 * than in the NavHost, so it survives navigation the way the web client's fixed
 * footer does.
 */
@Composable
fun MiniPlayer(
	state: PlayerState,
	onExpand: () -> Unit,
	onTogglePlay: () -> Unit,
	onNext: () -> Unit,
) {
	val current = state.current ?: return

	Surface(tonalElevation = 3.dp) {
		Column {
			LinearProgressIndicator(
				progress = { progressOf(state) },
				modifier = Modifier.fillMaxWidth(),
			)
			Row(
				modifier = Modifier
					.fillMaxWidth()
					.clickable(onClick = onExpand)
					.padding(horizontal = 12.dp, vertical = 8.dp),
				verticalAlignment = Alignment.CenterVertically,
				horizontalArrangement = Arrangement.spacedBy(12.dp),
			) {
				CoverThumb(current.artworkUrl, current.album, size = 40.dp)

				Column(modifier = Modifier.weight(1f)) {
					Text(
						text = current.title,
						style = MaterialTheme.typography.bodyMedium,
						maxLines = 1,
						overflow = TextOverflow.Ellipsis,
					)
					Text(
						text = current.artist,
						style = MaterialTheme.typography.bodySmall,
						color = MaterialTheme.colorScheme.onSurfaceVariant,
						maxLines = 1,
						overflow = TextOverflow.Ellipsis,
					)
				}

				IconButton(onClick = onTogglePlay) {
					Icon(
						imageVector = if (state.isPlaying) {
							Icons.Default.Pause
						} else {
							Icons.Default.PlayArrow
						},
						contentDescription = if (state.isPlaying) "Pause" else "Play",
					)
				}
				IconButton(onClick = onNext, enabled = state.hasNext) {
					Icon(Icons.Default.SkipNext, contentDescription = "Next")
				}
			}
		}
	}
}

/** Zero rather than NaN before the player knows the duration. */
internal fun progressOf(state: PlayerState): Float =
	if (state.durationMs <= 0) 0f
	else (state.positionMs.toFloat() / state.durationMs).coerceIn(0f, 1f)
