package org.gaindrive.android.ui.player

import androidx.compose.foundation.background
import androidx.compose.foundation.clickable
import androidx.compose.foundation.interaction.MutableInteractionSource
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.fillMaxHeight
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.shape.CircleShape
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.automirrored.filled.VolumeDown
import androidx.compose.material.icons.automirrored.filled.VolumeUp
import androidx.compose.material.icons.filled.ArrowDropDown
import androidx.compose.material.icons.filled.Check
import androidx.compose.material.icons.filled.Delete
import androidx.compose.material.icons.filled.Pause
import androidx.compose.material.icons.filled.PlayArrow
import androidx.compose.material.icons.filled.Save
import androidx.compose.material.icons.filled.SkipNext
import androidx.compose.material.icons.filled.SkipPrevious
import androidx.compose.material3.DropdownMenu
import androidx.compose.material3.DropdownMenuItem
import androidx.compose.material3.ExperimentalMaterial3Api
import androidx.compose.material3.HorizontalDivider
import androidx.compose.material3.Icon
import androidx.compose.material3.LinearProgressIndicator
import androidx.compose.material3.IconButton
import androidx.compose.material3.LocalMinimumInteractiveComponentSize
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.OutlinedButton
import androidx.compose.material3.OutlinedTextField
import androidx.compose.material3.RadioButton
import androidx.compose.material3.Slider
import androidx.compose.material3.SliderDefaults
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.CompositionLocalProvider
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.graphics.graphicsLayer
import androidx.compose.ui.layout.layout
import androidx.compose.ui.text.style.TextAlign
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.Constraints
import androidx.compose.ui.unit.DpSize
import androidx.compose.ui.unit.dp
import kotlin.math.roundToInt

/**
 * The pieces the sound-control sheets share: a fader with its readout and
 * label, the web panel's preset row, and the volume row.
 *
 * [EqualizerSheet] shapes the phone's own audio path and [WiiMControlsSheet]
 * a WiiM's, and their scales differ (millibels against the WiiM's 0..99), so
 * everything here speaks plain floats and strings and each sheet does its own
 * translating.
 */

@Composable
internal fun BandColumn(
	readout: String,
	label: String,
	value: Float,
	valueRange: ClosedFloatingPointRange<Float>,
	steps: Int,
	enabled: Boolean,
	onValue: (Float) -> Unit,
	onDone: () -> Unit,
) {
	Column(horizontalAlignment = Alignment.CenterHorizontally) {
		Text(
			text = readout,
			style = MaterialTheme.typography.labelSmall,
		)
		VerticalSlider(
			value = value,
			onValueChange = onValue,
			onValueChangeFinished = onDone,
			valueRange = valueRange,
			steps = steps,
			enabled = enabled,
			modifier = Modifier.height(FADER_HEIGHT),
		)
		Text(
			text = label,
			style = MaterialTheme.typography.labelSmall,
			color = MaterialTheme.colorScheme.onSurfaceVariant,
		)
	}
}

/**
 * The web panel's preset row: a dropdown that reads "Custom" once a fader has
 * moved, a save button revealing an inline name field, and a trashcan that
 * only works on something the user saved.
 */
@Composable
internal fun PresetRow(
	preset: String?,
	presets: List<String>,
	saved: List<String>,
	enabled: Boolean,
	error: String?,
	onDevicePreset: (Int) -> Unit,
	onSavedPreset: (String) -> Unit,
	onSave: (String) -> Boolean,
	onDelete: () -> Unit,
	onClearError: () -> Unit,
	// False when there is no curve to save: a WiiM whose firmware reports no
	// band values still gets the dropdown, but a save button there could only
	// refuse.
	savable: Boolean = true,
) {
	var menuOpen by remember { mutableStateOf(false) }
	var saving by remember { mutableStateOf(false) }
	var saveName by remember { mutableStateOf("") }

	Row(
		modifier = Modifier
			.fillMaxWidth()
			.padding(start = 24.dp, end = 12.dp, top = 8.dp),
		verticalAlignment = Alignment.CenterVertically,
	) {
		Box(modifier = Modifier.weight(1f)) {
			// The app's labelled-picker idiom (the fetch panel's library
			// picker): a button naming the choice, over a DropdownMenu.
			OutlinedButton(onClick = { menuOpen = true }, enabled = enabled) {
				Text(
					text = preset ?: "Custom",
					maxLines = 1,
					overflow = TextOverflow.Ellipsis,
				)
				Icon(Icons.Default.ArrowDropDown, contentDescription = null)
			}
			DropdownMenu(expanded = menuOpen, onDismissRequest = { menuOpen = false }) {
				presets.forEachIndexed { index, name ->
					DropdownMenuItem(
						text = { Text(name) },
						leadingIcon = {
							RadioButton(selected = name == preset, onClick = null)
						},
						onClick = {
							menuOpen = false
							onDevicePreset(index)
						},
					)
				}
				// The saved section, when there is one; a divider rather
				// than a heading, the way the library selector groups.
				if (saved.isNotEmpty()) {
					HorizontalDivider()
					saved.forEach { name ->
						DropdownMenuItem(
							text = { Text(name) },
							leadingIcon = {
								RadioButton(selected = name == preset, onClick = null)
							},
							onClick = {
								menuOpen = false
								onSavedPreset(name)
							},
						)
					}
				}
			}
		}
		if (savable) {
			IconButton(onClick = { saving = !saving }, enabled = enabled) {
				Icon(Icons.Default.Save, contentDescription = "Save preset")
			}
			IconButton(
				onClick = onDelete,
				// Only what the user saved can be deleted; the device's own
				// presets and an unsaved custom curve cannot.
				enabled = enabled && preset?.let { it in saved } == true,
			) {
				Icon(Icons.Default.Delete, contentDescription = "Delete preset")
			}
		}
	}

	if (saving && enabled) {
		// Why the save was refused; above the field it belongs to, tapping
		// clears it, like the WiiM sheet's error line.
		error?.let {
			Text(
				text = it,
				style = MaterialTheme.typography.bodySmall,
				color = MaterialTheme.colorScheme.error,
				modifier = Modifier
					.clickable(onClick = onClearError)
					.padding(start = 24.dp, end = 24.dp, top = 4.dp),
			)
		}
		Row(
			modifier = Modifier
				.fillMaxWidth()
				.padding(start = 24.dp, end = 12.dp, top = 4.dp),
			verticalAlignment = Alignment.CenterVertically,
			horizontalArrangement = Arrangement.spacedBy(8.dp),
		) {
			OutlinedTextField(
				value = saveName,
				onValueChange = {
					saveName = it
					onClearError()
				},
				label = { Text("Preset name") },
				singleLine = true,
				modifier = Modifier.weight(1f),
			)
			IconButton(
				onClick = {
					if (onSave(saveName)) {
						saving = false
						saveName = ""
					}
				},
				enabled = saveName.isNotBlank(),
			) {
				Icon(Icons.Default.Check, contentDescription = "Save as preset")
			}
		}
	}
}

/**
 * Volume up and down around an indicator, sized for a thumb: the sheet is
 * reached mid-listening, often at arm's length from the speaker it controls.
 *
 * [fraction] is 0..1, or null while the level is not yet known (a cast
 * receiver that has not answered its first GET_STATUS); the buttons disable
 * rather than guess.
 */
@Composable
internal fun VolumeRow(
	fraction: Float?,
	onDown: () -> Unit,
	onUp: () -> Unit,
) {
	Column(modifier = Modifier.padding(start = 24.dp, end = 24.dp, top = 8.dp)) {
		Text(text = "Volume", style = MaterialTheme.typography.titleSmall)
		// The bar sits directly in the row so the buttons centre on it; the
		// readout hangs below the whole row, where the equal-width buttons on
		// either side keep it centred under the bar.
		Row(verticalAlignment = Alignment.CenterVertically) {
			IconButton(
				onClick = onDown,
				enabled = fraction != null,
				modifier = Modifier.size(VOLUME_BUTTON_SIZE),
			) {
				Icon(
					Icons.AutoMirrored.Filled.VolumeDown,
					contentDescription = "Volume down",
					modifier = Modifier.size(VOLUME_ICON_SIZE),
				)
			}
			LinearProgressIndicator(
				progress = { fraction ?: 0f },
				modifier = Modifier.weight(1f).padding(horizontal = 8.dp),
			)
			IconButton(
				onClick = onUp,
				enabled = fraction != null,
				modifier = Modifier.size(VOLUME_BUTTON_SIZE),
			) {
				Icon(
					Icons.AutoMirrored.Filled.VolumeUp,
					contentDescription = "Volume up",
					modifier = Modifier.size(VOLUME_ICON_SIZE),
				)
			}
		}
		Text(
			text = fraction?.let { "${(it * 100).roundToInt()}%" } ?: "",
			style = MaterialTheme.typography.labelSmall,
			textAlign = TextAlign.Center,
			modifier = Modifier.fillMaxWidth(),
		)
	}
}

/**
 * Previous, play/pause, next, repeating the Now Playing sheet's transport in
 * its sizes: these sheets are reached mid-listening, and closing one just to
 * skip a track is the round trip this saves.
 */
@Composable
internal fun TransportRow(
	playing: Boolean,
	hasPrevious: Boolean,
	hasNext: Boolean,
	onPrevious: () -> Unit,
	onTogglePlay: () -> Unit,
	onNext: () -> Unit,
) {
	Row(
		modifier = Modifier.fillMaxWidth(),
		horizontalArrangement = Arrangement.Center,
		verticalAlignment = Alignment.CenterVertically,
	) {
		IconButton(onClick = onPrevious, enabled = hasPrevious) {
			Icon(
				Icons.Default.SkipPrevious,
				contentDescription = "Previous",
				modifier = Modifier.size(36.dp),
			)
		}
		IconButton(onClick = onTogglePlay) {
			Icon(
				imageVector = if (playing) Icons.Default.Pause else Icons.Default.PlayArrow,
				contentDescription = if (playing) "Pause" else "Play",
				modifier = Modifier.size(48.dp),
			)
		}
		IconButton(onClick = onNext, enabled = hasNext) {
			Icon(
				Icons.Default.SkipNext,
				contentDescription = "Next",
				modifier = Modifier.size(36.dp),
			)
		}
	}
}

/**
 * A [Slider] stood on end, minimum at the bottom, drawn the way the WiiM app
 * draws a fader: a thin bar with a ball riding on it.
 *
 * Material3 has no vertical slider, so this is the usual recipe: rotate the
 * drawn (and touched) layer a quarter turn, and swap the measurement
 * constraints so the slider lays itself out along what the parent considers
 * height. The placement offsets re-centre the rotated box, whose reported
 * size has its sides exchanged.
 *
 * The looks are the [SeekBar] situation taken one step further, and its
 * constraint note (on `THUMB_SIZE`) is the map. The thumb is Material's own
 * at a square size, which its half-width corner radius turns into the ball
 * while keeping the press animation. The track is drawn by hand because
 * `SliderDefaults.Track` fixes its own height internally; no gap is drawn
 * around the thumb, since the ball riding on the bar is the point. And the
 * interactive size is lowered from Material's 44dp, because a rotated
 * slider's height is the column width on screen and ten of these must share
 * a portrait phone; the WiiM app makes the same touch-target tradeoff.
 */
@OptIn(ExperimentalMaterial3Api::class)
@Composable
private fun VerticalSlider(
	value: Float,
	onValueChange: (Float) -> Unit,
	onValueChangeFinished: () -> Unit,
	valueRange: ClosedFloatingPointRange<Float>,
	steps: Int,
	enabled: Boolean,
	modifier: Modifier = Modifier,
) {
	// Shared with the thumb for SeekBar's reason: a thumb that remembers its
	// own source never hears about the drag, and its press feedback silently
	// stops happening.
	val interactionSource = remember { MutableInteractionSource() }
	CompositionLocalProvider(LocalMinimumInteractiveComponentSize provides FADER_TOUCH_WIDTH) {
		Slider(
			value = value,
			onValueChange = onValueChange,
			onValueChangeFinished = onValueChangeFinished,
			valueRange = valueRange,
			steps = steps,
			enabled = enabled,
			interactionSource = interactionSource,
			thumb = {
				SliderDefaults.Thumb(
					interactionSource = interactionSource,
					enabled = enabled,
					thumbSize = DpSize(THUMB_DIAMETER, THUMB_DIAMETER),
				)
			},
			track = { state ->
				val range = state.valueRange.endInclusive - state.valueRange.start
				val fraction =
					if (range > 0f) ((state.value - state.valueRange.start) / range).coerceIn(0f, 1f)
					else 0f
				val active =
					if (enabled) MaterialTheme.colorScheme.primary
					else MaterialTheme.colorScheme.onSurface.copy(alpha = 0.38f)
				val inactive =
					if (enabled) MaterialTheme.colorScheme.surfaceVariant
					else MaterialTheme.colorScheme.onSurface.copy(alpha = 0.12f)
				Box(
					modifier = Modifier
						.fillMaxWidth()
						.height(TRACK_THICKNESS)
						.background(inactive, CircleShape),
				) {
					Box(
						modifier = Modifier
							.fillMaxWidth(fraction)
							.fillMaxHeight()
							.background(active, CircleShape),
					)
				}
			},
			modifier = modifier
				.graphicsLayer { rotationZ = 270f }
				.layout { measurable, constraints ->
					val placeable = measurable.measure(
						Constraints(
							minWidth = constraints.minHeight,
							maxWidth = constraints.maxHeight,
							minHeight = constraints.minWidth,
							maxHeight = constraints.maxWidth,
						)
					)
					layout(placeable.height, placeable.width) {
						placeable.place(
							x = (placeable.height - placeable.width) / 2,
							y = (placeable.width - placeable.height) / 2,
						)
					}
				},
		)
	}
}

internal val FADER_HEIGHT = 160.dp

private val VOLUME_BUTTON_SIZE = 56.dp
private val VOLUME_ICON_SIZE = 32.dp

/** The on-screen column width; the single knob if dragging feels too fiddly. */
private val FADER_TOUCH_WIDTH = 28.dp

private val TRACK_THICKNESS = 3.dp
private val THUMB_DIAMETER = 16.dp
