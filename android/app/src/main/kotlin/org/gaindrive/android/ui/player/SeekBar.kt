package org.gaindrive.android.ui.player

import androidx.compose.foundation.interaction.MutableInteractionSource
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.padding
import androidx.compose.material3.ExperimentalMaterial3Api
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Slider
import androidx.compose.material3.SliderDefaults
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableFloatStateOf
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.Modifier
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.unit.DpSize
import androidx.compose.ui.unit.dp
import org.gaindrive.android.playback.PlayerState
import org.gaindrive.android.ui.components.formatDuration
import org.gaindrive.android.ui.tvFocusHighlight

/**
 * The scrub bar, shared by the Now Playing sheet, the video screen and the
 * mini player where a tablet leaves room for one.
 *
 * While the user is dragging, the bar shows their finger rather than the
 * player's position - otherwise every position poll would yank the thumb back
 * under them.
 *
 * [textColor] exists for the video screen, where the controls sit over the
 * picture and have to be legible against it rather than against a surface.
 *
 * The handle is Material's own, at [THUMB_SIZE] rather than its own size.
 */
@OptIn(ExperimentalMaterial3Api::class)
@Composable
fun SeekBar(
	state: PlayerState,
	onSeek: (Long) -> Unit,
	modifier: Modifier = Modifier.padding(horizontal = 24.dp),
	textColor: Color = MaterialTheme.colorScheme.onSurfaceVariant,
) {
	var dragging by remember { mutableStateOf(false) }
	var dragFraction by remember { mutableFloatStateOf(0f) }
	// Shared with the thumb below rather than left to each of them. A thumb
	// that remembers its own never hears about the drag, and the one piece of
	// feedback Material's handle offers - narrowing while it is held - then
	// silently stops happening.
	val interactionSource = remember { MutableInteractionSource() }
	val enabled = state.durationMs > 0

	val fraction = if (dragging) dragFraction else progressOf(state)
	val shownPositionMs =
		if (dragging) (dragFraction * state.durationMs).toLong() else state.positionMs

	Column(modifier = modifier) {
		Slider(
			// The slider steps on d-pad left/right by itself once focused; the
			// highlight is what makes it visible that it is the thing focused.
			modifier = Modifier.tvFocusHighlight(),
			value = fraction,
			onValueChange = {
				dragging = true
				dragFraction = it
			},
			onValueChangeFinished = {
				dragging = false
				onSeek((dragFraction * state.durationMs).toLong())
			},
			enabled = enabled,
			interactionSource = interactionSource,
			thumb = {
				SliderDefaults.Thumb(
					interactionSource = interactionSource,
					// Passed on rather than left to default, or a track with
					// no duration yet draws dead under a live handle.
					enabled = enabled,
					thumbSize = THUMB_SIZE,
				)
			},
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

/**
 * The scrub handle.
 *
 * Material 3's own is 4x44dp against a 16dp track - nearly three times its
 * height, and about as tall as the whole mini-player row, where it reads as a
 * bar drawn *over* the player rather than a handle on it. At 24dp it clears the
 * track by 4dp either side instead of 14.
 *
 * **The width is left at Material's 4dp deliberately.** The track draws its gap
 * around a handle of that width, so narrowing it would leave a gap wider than
 * the thing sitting in it.
 *
 * Only the handle can be sized this way, and it is only the drawing that
 * changes: `Slider` keeps a 44dp interactive height whatever thumb it is given,
 * and `SliderDefaults.Track` fixes its own height internally, so a modifier
 * aimed at it is overridden. A shorter row, or a thinner track, means drawing
 * both by hand and reimplementing the gap, the stop indicator and the press
 * animation - which is why neither was done here.
 */
private val THUMB_SIZE = DpSize(4.dp, 24.dp)
