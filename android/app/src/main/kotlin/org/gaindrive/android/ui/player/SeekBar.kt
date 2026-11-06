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
import androidx.compose.ui.focus.FocusRequester
import androidx.compose.ui.focus.focusRequester
import androidx.compose.ui.input.key.Key
import androidx.compose.ui.input.key.KeyEventType
import androidx.compose.ui.input.key.key
import androidx.compose.ui.input.key.onPreviewKeyEvent
import androidx.compose.ui.input.key.type
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.unit.DpSize
import androidx.compose.ui.unit.dp
import org.gaindrive.android.data.model.Chapter
import org.gaindrive.android.playback.PlayerState
import org.gaindrive.android.ui.LocalIsTv
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
 *
 * On TV, d-pad left and right skip by [SKIP_MS] rather than the slider's own
 * 1% step, and center runs [onPlayPause] when given: Play's TV review wants
 * exactly that of a playing screen (TV-PC). [focusRequester] lets a screen
 * land the d-pad here first.
 *
 * [window], from [chapterWindow], narrows the bar to one chapter: thumb,
 * labels and seeks are all relative to it. Null is the whole item.
 */
@OptIn(ExperimentalMaterial3Api::class)
@Composable
fun SeekBar(
	state: PlayerState,
	onSeek: (Long) -> Unit,
	modifier: Modifier = Modifier.padding(horizontal = 24.dp),
	textColor: Color = MaterialTheme.colorScheme.onSurfaceVariant,
	onPlayPause: (() -> Unit)? = null,
	focusRequester: FocusRequester? = null,
	window: LongRange? = null,
) {
	val isTv = LocalIsTv.current
	var dragging by remember { mutableStateOf(false) }
	var dragFraction by remember { mutableFloatStateOf(0f) }
	// Shared with the thumb below rather than left to each of them. A thumb
	// that remembers its own never hears about the drag, and the one piece of
	// feedback Material's handle offers - narrowing while it is held - then
	// silently stops happening.
	val interactionSource = remember { MutableInteractionSource() }
	val enabled = state.durationMs > 0
	val startMs = window?.first ?: 0L
	val spanMs = window?.let { it.last - it.first } ?: state.durationMs
	val endMs = startMs + spanMs

	val fraction = if (dragging) dragFraction else progressOf(state, window)
	val shownPositionMs =
		if (dragging) (dragFraction * spanMs).toLong() else state.positionMs - startMs

	Column(modifier = modifier) {
		Slider(
			// The highlight is what makes it visible that the slider is the
			// thing focused. The key up is eaten along with the down: the
			// slider would otherwise finish a keyboard "drag" it never began
			// and seek to wherever its stale drag fraction points.
			modifier = Modifier
				.then(focusRequester?.let { Modifier.focusRequester(it) } ?: Modifier)
				.onPreviewKeyEvent { event ->
					if (!isTv || !enabled) return@onPreviewKeyEvent false
					val down = event.type == KeyEventType.KeyDown
					when (event.key) {
						Key.DirectionLeft, Key.DirectionRight -> {
							if (down) {
								val step = if (event.key == Key.DirectionLeft) -SKIP_MS else SKIP_MS
								onSeek((state.positionMs + step).coerceIn(startMs, endMs))
							}
							true
						}
						Key.DirectionCenter, Key.Enter -> {
							val toggle = onPlayPause ?: return@onPreviewKeyEvent false
							if (down) toggle()
							true
						}
						else -> false
					}
				}
				.tvFocusHighlight(),
			value = fraction,
			onValueChange = {
				dragging = true
				dragFraction = it
			},
			onValueChangeFinished = {
				dragging = false
				onSeek(startMs + (dragFraction * spanMs).toLong())
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
				text = formatDuration((shownPositionMs.coerceAtLeast(0) / 1000).toInt()),
				style = MaterialTheme.typography.labelSmall,
				color = textColor,
			)
			Text(
				text = formatDuration((spanMs / 1000).toInt()),
				style = MaterialTheme.typography.labelSmall,
				color = textColor,
			)
		}
	}
}

/**
 * The chapter [positionMs] is in, from its start to the next one's or the end
 * of the item, for [SeekBar]'s `window`. Null with no markers, or before the
 * duration is known.
 *
 * Before the first marker counts as the first chapter, so a film whose first
 * marker is not at zero still gets a window from its very start.
 */
fun chapterWindow(chapters: List<Chapter>, positionMs: Long, durationMs: Long): LongRange? {
	if (chapters.isEmpty() || durationMs <= 0) return null
	val starts = chapters.map { it.startMs }.sorted()
	val i = starts.indexOfLast { it <= positionMs }.coerceAtLeast(0)
	val start = if (i == 0) 0L else starts[i]
	val end = starts.getOrNull(i + 1) ?: durationMs
	return if (end > start) start..end else null
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
