package org.gaindrive.android.ui.player

import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.padding
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Slider
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableFloatStateOf
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.Modifier
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.unit.dp
import org.gaindrive.android.playback.PlayerState
import org.gaindrive.android.ui.components.formatDuration

/**
 * The scrub bar, shared by the Now Playing sheet and the video screen.
 *
 * While the user is dragging, the bar shows their finger rather than the
 * player's position — otherwise every position poll would yank the thumb back
 * under them.
 *
 * [textColor] exists for the video screen, where the controls sit over the
 * picture and have to be legible against it rather than against a surface.
 */
@Composable
fun SeekBar(
	state: PlayerState,
	onSeek: (Long) -> Unit,
	modifier: Modifier = Modifier.padding(horizontal = 24.dp),
	textColor: Color = MaterialTheme.colorScheme.onSurfaceVariant,
) {
	var dragging by remember { mutableStateOf(false) }
	var dragFraction by remember { mutableFloatStateOf(0f) }

	val fraction = if (dragging) dragFraction else progressOf(state)
	val shownPositionMs =
		if (dragging) (dragFraction * state.durationMs).toLong() else state.positionMs

	Column(modifier = modifier) {
		Slider(
			value = fraction,
			onValueChange = {
				dragging = true
				dragFraction = it
			},
			onValueChangeFinished = {
				dragging = false
				onSeek((dragFraction * state.durationMs).toLong())
			},
			enabled = state.durationMs > 0,
		)
		Row(
			modifier = Modifier.fillMaxWidth(),
			horizontalArrangement = Arrangement.SpaceBetween,
		) {
			Text(
				text = formatDuration((shownPositionMs / 1000).toInt()),
				style = MaterialTheme.typography.labelSmall,
				color = textColor,
			)
			Text(
				text = formatDuration((state.durationMs / 1000).toInt()),
				style = MaterialTheme.typography.labelSmall,
				color = textColor,
			)
		}
	}
}
