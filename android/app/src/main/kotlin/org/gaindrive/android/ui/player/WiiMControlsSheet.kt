package org.gaindrive.android.ui.player

import androidx.compose.foundation.clickable
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
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.getValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.dp
import androidx.hilt.lifecycle.viewmodel.compose.hiltViewModel
import androidx.lifecycle.compose.collectAsStateWithLifecycle
import org.gaindrive.android.playback.wiim.WIIM_BANDS
import org.gaindrive.android.playback.wiim.WIIM_LEVEL_FLAT
import org.gaindrive.android.playback.wiim.WIIM_LEVEL_MAX
import org.gaindrive.android.playback.wiim.WIIM_LEVEL_MIN
import org.gaindrive.android.ui.components.LoadStateBox
import kotlin.math.roundToInt

/**
 * The controls a WiiM has that the Cast protocol does not reach.
 *
 * Today that is the equalizer: the switch, the device's ten graphic EQ
 * faders, and the preset row shared with [EqualizerSheet] via `EqPanel.kt`.
 * The volume row below them is the exception that proves the shape: it rides
 * the Cast channel ([CastVolumeSection]), not this API. The sheet is still
 * shaped as a list of sections rather than as an EQ screen because the same
 * API carries input switching and the device's own volume model, and those
 * belong here when they land.
 *
 * A firmware that reports no band values (the `EQGetStat` fallback) degrades
 * to the switch and the preset dropdown; saving needs a curve to save.
 *
 * A second [ModalBottomSheet] beside the Now Playing one, hosted from
 * `GainDriveApp` exactly as [CastDeviceSheet] is — not a sheet opened from
 * inside another, which `TrackActionsSheet` explains this codebase avoids.
 */
@OptIn(ExperimentalMaterial3Api::class)
@Composable
fun WiiMControlsSheet(
	onDismiss: () -> Unit,
	viewModel: WiiMControlsViewModel = hiltViewModel(),
) {
	val device by viewModel.device.collectAsStateWithLifecycle()
	val state by viewModel.state.collectAsStateWithLifecycle()
	val busy by viewModel.busy.collectAsStateWithLifecycle()
	val error by viewModel.error.collectAsStateWithLifecycle()
	val sheetState = rememberModalBottomSheetState(skipPartiallyExpanded = true)

	// Keyed on the device so switching speakers with the sheet open re-reads
	// rather than showing the previous one's equalizer.
	LaunchedEffect(device?.address) { viewModel.refresh() }

	ModalBottomSheet(onDismissRequest = onDismiss, sheetState = sheetState) {
		Column(modifier = Modifier.padding(bottom = 24.dp)) {
			Text(
				text = device?.name ?: "WiiM",
				style = MaterialTheme.typography.titleMedium,
				maxLines = 1,
				overflow = TextOverflow.Ellipsis,
				modifier = Modifier.padding(start = 24.dp, end = 24.dp),
			)
			device?.model?.let {
				Text(
					text = it,
					style = MaterialTheme.typography.bodySmall,
					color = MaterialTheme.colorScheme.onSurfaceVariant,
					modifier = Modifier.padding(start = 24.dp, end = 24.dp),
				)
			}

			// Above the panel rather than inside it, because a failed command
			// is about the device and not about one control. Tapping clears it.
			error?.let {
				Text(
					text = it,
					style = MaterialTheme.typography.bodySmall,
					color = MaterialTheme.colorScheme.error,
					modifier = Modifier
						.clickable(onClick = viewModel::clearError)
						.padding(start = 24.dp, end = 24.dp, top = 12.dp),
				)
			}

			// LoadStateBox fills whatever it is given, and a bottom sheet's
			// Column is as tall as its content — so the height has to be stated
			// or the panel would run off the bottom of the sheet. A fixed block
			// rather than a bound is the better of the two: the spinner then
			// sits in the space the panel is about to occupy, and the sheet
			// does not jump when it arrives.
			LoadStateBox(
				state = state,
				onRetry = viewModel::refresh,
				modifier = Modifier.heightIn(max = PANEL_HEIGHT),
			) { eq ->
				// Scrollable for EqualizerSheet's reason: with the keyboard up
				// the sheet has less height than the panel, and a scroll keeps
				// the children at their intrinsic heights instead of clipping
				// the name field.
				Column(modifier = Modifier.verticalScroll(rememberScrollState())) {
					Row(
						modifier = Modifier
							.fillMaxWidth()
							.padding(start = 24.dp, end = 24.dp, top = 16.dp, bottom = 8.dp),
						verticalAlignment = Alignment.CenterVertically,
						horizontalArrangement = Arrangement.SpaceBetween,
					) {
						Text(text = "Equalizer", style = MaterialTheme.typography.titleSmall)
						Switch(
							checked = eq.enabled,
							onCheckedChange = viewModel::setEnabled,
							enabled = !busy,
						)
					}
					eq.bands?.let { bands ->
						// Ten across with no sideways scrolling, like the
						// WiiM app; the faders are thin enough for that (see
						// VerticalSlider in EqPanel.kt).
						Row(
							modifier = Modifier
								.fillMaxWidth()
								.padding(horizontal = 16.dp),
							horizontalArrangement = Arrangement.SpaceEvenly,
						) {
							bands.forEachIndexed { index, value ->
								BandColumn(
									// An offset from flat, not decibels: how
									// the device's 0..99 maps to dB is
									// unverified, and a figure the app cannot
									// vouch for is worse than a relative one.
									readout = "%+d".format(value - WIIM_LEVEL_FLAT),
									label = WIIM_BANDS[index].second,
									value = value.toFloat(),
									valueRange = WIIM_LEVEL_MIN.toFloat()..WIIM_LEVEL_MAX.toFloat(),
									// Continuous: 98 detents would be invisible,
									// and with no verified dB mapping there is
									// no natural grid to snap to.
									steps = 0,
									// Disabled, not merely dimmed, while the
									// switch is off: a fader that still moves
									// suggests it still does something.
									enabled = eq.enabled && !busy,
									onValue = { viewModel.setBand(index, it.roundToInt()) },
									onDone = viewModel::commitBands,
								)
							}
						}
					}
					// Enabled while the switch is off, unlike the faders:
					// loading a preset switches the equalizer on, so the
					// dropdown is a way in, not a dead control.
					PresetRow(
						preset = eq.preset,
						presets = eq.presets,
						saved = eq.saved,
						enabled = !busy,
						// Null: this sheet's top line already shows every
						// error, and a save refusal rendered twice reads as
						// two problems.
						error = null,
						onDevicePreset = { viewModel.selectPreset(eq.presets[it]) },
						onSavedPreset = viewModel::applySaved,
						onSave = viewModel::save,
						onDelete = viewModel::deleteSlot,
						onClearError = viewModel::clearError,
						savable = eq.bands != null,
					)
				}
			}
			// Outside the load box on purpose: transport and volume ride the
			// Cast channel, so they work even when the HTTP equalizer read
			// failed, and they must not sit behind the EQ spinner.
			TransportSection()
			CastVolumeSection()
		}
	}
}

/**
 * The switch row, the faders with their labels, the preset row, and the name
 * field the save button reveals under it; fixed so the loading state occupies
 * the space the panel will.
 */
private val PANEL_HEIGHT = 440.dp
