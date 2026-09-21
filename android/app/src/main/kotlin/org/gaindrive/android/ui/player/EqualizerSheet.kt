package org.gaindrive.android.ui.player

import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.heightIn
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.verticalScroll
import androidx.compose.material3.ExperimentalMaterial3Api
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.ModalBottomSheet
import androidx.compose.material3.Switch
import androidx.compose.material3.Text
import androidx.compose.material3.rememberModalBottomSheetState
import androidx.compose.runtime.Composable
import androidx.compose.runtime.getValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.unit.dp
import androidx.hilt.lifecycle.viewmodel.compose.hiltViewModel
import androidx.lifecycle.compose.collectAsStateWithLifecycle
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
 * `GainDriveApp` exactly as [WiiMControlsSheet] is. The faders and the preset
 * row are the shared ones in `EqPanel.kt`.
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
							readout = "%+d".format((band.levelMb / 100.0).roundToInt()),
							label = freqLabel(band.centerHz),
							value = band.levelMb.toFloat(),
							valueRange = eq.minMb.toFloat()..eq.maxMb.toFloat(),
							// Detents at whole decibels; the level range speaks
							// millibels.
							steps = ((eq.maxMb - eq.minMb) / 100 - 1).coerceAtLeast(0),
							// Disabled, not merely dimmed, while the switch is
							// off: a fader that still moves suggests it still
							// does something.
							enabled = eq.enabled,
							onValue = { viewModel.setBandLevel(index, it.roundToInt()) },
							onDone = viewModel::commitLevels,
						)
					}
				}
				PresetRow(
					preset = eq.preset,
					presets = eq.presets,
					saved = eq.saved,
					enabled = eq.enabled,
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

private fun freqLabel(hz: Int): String = when {
	hz < 1000 -> "$hz Hz"
	hz % 1000 == 0 -> "${hz / 1000} kHz"
	else -> "%.1f kHz".format(hz / 1000.0)
}

/** The switch row, the faders with their labels, the preset row, and the
 * name field the save button reveals under it. */
private val PANEL_HEIGHT = 440.dp
