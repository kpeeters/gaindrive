package org.gaindrive.android.ui.player

import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.heightIn
import androidx.compose.foundation.layout.padding
import androidx.compose.material3.ExperimentalMaterial3Api
import androidx.compose.material3.FilterChip
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.ModalBottomSheet
import androidx.compose.material3.Slider
import androidx.compose.material3.Switch
import androidx.compose.material3.Text
import androidx.compose.material3.rememberModalBottomSheetState
import androidx.compose.runtime.Composable
import androidx.compose.runtime.getValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.graphics.graphicsLayer
import androidx.compose.ui.layout.layout
import androidx.compose.ui.unit.Constraints
import androidx.compose.ui.unit.dp
import androidx.hilt.navigation.compose.hiltViewModel
import androidx.lifecycle.compose.collectAsStateWithLifecycle
import org.gaindrive.android.playback.EqBand
import org.gaindrive.android.ui.components.ChipRow
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
			Column {
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
				// ChipRow scrolls sideways and finds the selected chip, which
				// is what a list of a dozen device presets needs.
				ChipRow(
					selectedIndex = eq.preset?.let(eq.presets::indexOf) ?: -1,
					chipCount = eq.presets.size,
					modifier = Modifier.padding(horizontal = 24.dp, vertical = 8.dp),
				) {
					eq.presets.forEachIndexed { index, name ->
						FilterChip(
							selected = name == eq.preset,
							enabled = eq.enabled,
							onClick = { viewModel.usePreset(index) },
							label = { Text(name, maxLines = 1) },
						)
					}
				}
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

/** The switch row, the faders with their labels, and the preset chips. */
private val PANEL_HEIGHT = 340.dp

private val FADER_HEIGHT = 160.dp
