package org.gaindrive.android.ui.player

import androidx.compose.foundation.clickable
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.heightIn
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.verticalScroll
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.filled.ArrowDropDown
import androidx.compose.material.icons.filled.Check
import androidx.compose.material.icons.filled.Delete
import androidx.compose.material.icons.filled.Save
import androidx.compose.material3.DropdownMenu
import androidx.compose.material3.DropdownMenuItem
import androidx.compose.material3.ExperimentalMaterial3Api
import androidx.compose.material3.HorizontalDivider
import androidx.compose.material3.Icon
import androidx.compose.material3.IconButton
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.ModalBottomSheet
import androidx.compose.material3.OutlinedButton
import androidx.compose.material3.OutlinedTextField
import androidx.compose.material3.RadioButton
import androidx.compose.material3.Slider
import androidx.compose.material3.Switch
import androidx.compose.material3.Text
import androidx.compose.material3.rememberModalBottomSheetState
import androidx.compose.runtime.Composable
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.graphics.graphicsLayer
import androidx.compose.ui.layout.layout
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.Constraints
import androidx.compose.ui.unit.dp
import androidx.hilt.lifecycle.viewmodel.compose.hiltViewModel
import androidx.lifecycle.compose.collectAsStateWithLifecycle
import org.gaindrive.android.playback.EqBand
import org.gaindrive.android.playback.EqState
import org.gaindrive.android.ui.components.LoadStateBox
import kotlin.math.roundToInt

/**
 * The local equalizer: `android.media.audiofx.Equalizer` on the phone's own
 * audio path.
 *
 * The bands are whatever the device reports, not the web client's fixed ten:
 * equalizer settings belong to the output device, and each sink keeps its own
 * band layout. This sheet is accordingly only offered while playing locally;
 * a WiiM's equalizer has [WiiMControlsSheet], and a plain Chromecast has
 * nothing to offer.
 *
 * A second [ModalBottomSheet] beside the Now Playing one, hosted from
 * `GainDriveApp` exactly as [WiiMControlsSheet] is.
 */
@OptIn(ExperimentalMaterial3Api::class)
@Composable
fun EqualizerSheet(
	onDismiss: () -> Unit,
	viewModel: EqualizerViewModel = hiltViewModel(),
) {
	val state by viewModel.state.collectAsStateWithLifecycle()
	val error by viewModel.error.collectAsStateWithLifecycle()
	val sheetState = rememberModalBottomSheetState(skipPartiallyExpanded = true)

	ModalBottomSheet(onDismissRequest = onDismiss, sheetState = sheetState) {
		// A stated height for the same reason WiiMControlsSheet states one:
		// LoadStateBox fills what it is given, and a sheet's Column gives an
		// unbounded nothing. Fixed rather than a bound, so the spinner and
		// the unavailable message occupy the space the panel will.
		LoadStateBox(
			state = state,
			modifier = Modifier
				.heightIn(max = PANEL_HEIGHT)
				.padding(bottom = 24.dp),
		) { eq ->
			// Scrollable so a shrunken viewport scrolls instead of squashing:
			// with the keyboard up the sheet has less height than the panel,
			// and a Column under a hard constraint hands its last child the
			// scraps, clipping the name field to them. Under a scroll the
			// children keep their intrinsic heights, and the focused field
			// asks to be brought into view on its own.
			Column(modifier = Modifier.verticalScroll(rememberScrollState())) {
				Row(
					modifier = Modifier
						.fillMaxWidth()
						.padding(start = 24.dp, end = 24.dp, top = 16.dp, bottom = 8.dp),
					verticalAlignment = Alignment.CenterVertically,
					horizontalArrangement = Arrangement.SpaceBetween,
				) {
					Text(text = "Equalizer", style = MaterialTheme.typography.titleSmall)
					Switch(checked = eq.enabled, onCheckedChange = viewModel::setEnabled)
				}
				Row(
					modifier = Modifier
						.fillMaxWidth()
						.padding(horizontal = 16.dp),
					horizontalArrangement = Arrangement.SpaceEvenly,
				) {
					eq.bands.forEachIndexed { index, band ->
						BandColumn(
							band = band,
							minMb = eq.minMb,
							maxMb = eq.maxMb,
							// Disabled, not merely dimmed, while the switch is
							// off: a fader that still moves suggests it still
							// does something.
							enabled = eq.enabled,
							onLevel = { viewModel.setBandLevel(index, it) },
							onDone = viewModel::commitLevels,
						)
					}
				}
				// The web panel's preset row: a dropdown that reads "Custom"
				// once a fader has moved, a save button, and a trashcan that
				// only works on something the user saved.
				PresetRow(
					eq = eq,
					error = error,
					onDevicePreset = viewModel::usePreset,
					onSavedPreset = viewModel::useSaved,
					onSave = viewModel::save,
					onDelete = viewModel::deleteSlot,
					onClearError = viewModel::clearError,
				)
			}
		}
	}
}

@Composable
private fun PresetRow(
	eq: EqState.Ready,
	error: String?,
	onDevicePreset: (Int) -> Unit,
	onSavedPreset: (String) -> Unit,
	onSave: (String) -> Boolean,
	onDelete: () -> Unit,
	onClearError: () -> Unit,
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
			OutlinedButton(onClick = { menuOpen = true }, enabled = eq.enabled) {
				Text(
					text = eq.preset ?: "Custom",
					maxLines = 1,
					overflow = TextOverflow.Ellipsis,
				)
				Icon(Icons.Default.ArrowDropDown, contentDescription = null)
			}
			DropdownMenu(expanded = menuOpen, onDismissRequest = { menuOpen = false }) {
				eq.presets.forEachIndexed { index, name ->
					DropdownMenuItem(
						text = { Text(name) },
						leadingIcon = {
							RadioButton(selected = name == eq.preset, onClick = null)
						},
						onClick = {
							menuOpen = false
							onDevicePreset(index)
						},
					)
				}
				// The saved section, when there is one; a divider rather
				// than a heading, the way the library selector groups.
				if (eq.saved.isNotEmpty()) {
					HorizontalDivider()
					eq.saved.forEach { name ->
						DropdownMenuItem(
							text = { Text(name) },
							leadingIcon = {
								RadioButton(selected = name == eq.preset, onClick = null)
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
		IconButton(onClick = { saving = !saving }, enabled = eq.enabled) {
			Icon(Icons.Default.Save, contentDescription = "Save preset")
		}
		IconButton(
			onClick = onDelete,
			// Only what the user saved can be deleted; the device's own
			// presets and an unsaved custom curve cannot.
			enabled = eq.enabled && eq.preset?.let { it in eq.saved } == true,
		) {
			Icon(Icons.Default.Delete, contentDescription = "Delete preset")
		}
	}

	if (saving && eq.enabled) {
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

@Composable
private fun BandColumn(
	band: EqBand,
	minMb: Int,
	maxMb: Int,
	enabled: Boolean,
	onLevel: (Int) -> Unit,
	onDone: () -> Unit,
) {
	Column(horizontalAlignment = Alignment.CenterHorizontally) {
		Text(
			text = "%+d".format((band.levelMb / 100.0).roundToInt()),
			style = MaterialTheme.typography.labelSmall,
		)
		VerticalSlider(
			value = band.levelMb.toFloat(),
			onValueChange = { onLevel(it.roundToInt()) },
			onValueChangeFinished = onDone,
			valueRange = minMb.toFloat()..maxMb.toFloat(),
			// Detents at whole decibels; the level range speaks millibels.
			steps = ((maxMb - minMb) / 100 - 1).coerceAtLeast(0),
			enabled = enabled,
			modifier = Modifier.height(FADER_HEIGHT),
		)
		Text(
			text = freqLabel(band.centerHz),
			style = MaterialTheme.typography.labelSmall,
			color = MaterialTheme.colorScheme.onSurfaceVariant,
		)
	}
}

/**
 * A [Slider] stood on end, minimum at the bottom.
 *
 * Material3 has no vertical slider, so this is the usual recipe: rotate the
 * drawn (and touched) layer a quarter turn, and swap the measurement
 * constraints so the slider lays itself out along what the parent considers
 * height. The placement offsets re-centre the rotated box, whose reported
 * size has its sides exchanged.
 */
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
	Slider(
		value = value,
		onValueChange = onValueChange,
		onValueChangeFinished = onValueChangeFinished,
		valueRange = valueRange,
		steps = steps,
		enabled = enabled,
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

private fun freqLabel(hz: Int): String = when {
	hz < 1000 -> "$hz Hz"
	hz % 1000 == 0 -> "${hz / 1000} kHz"
	else -> "%.1f kHz".format(hz / 1000.0)
}

/** The switch row, the faders with their labels, the preset row, and the
 * name field the save button reveals under it. */
private val PANEL_HEIGHT = 440.dp

private val FADER_HEIGHT = 160.dp
