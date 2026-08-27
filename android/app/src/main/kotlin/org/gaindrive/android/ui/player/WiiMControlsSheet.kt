package org.gaindrive.android.ui.player

import androidx.compose.foundation.clickable
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.fillMaxHeight
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.heightIn
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.lazy.items
import androidx.compose.foundation.lazy.rememberLazyListState
import androidx.compose.material3.ExperimentalMaterial3Api
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.ModalBottomSheet
import androidx.compose.material3.RadioButton
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
import androidx.hilt.navigation.compose.hiltViewModel
import androidx.lifecycle.compose.collectAsStateWithLifecycle
import org.gaindrive.android.ui.components.LoadStateBox

/**
 * The controls a WiiM has that the Cast protocol does not reach.
 *
 * Today that is the equalizer alone. The sheet is shaped as a list of sections
 * rather than as an EQ screen because the same API carries input switching and
 * the device's own volume model, and those belong here when they land.
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

			// Above the list rather than inside it, because a failed command is
			// about the device and not about one preset. Tapping clears it.
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
			// or a two-dozen-preset list would run off the bottom of the sheet.
			// A fixed block rather than a bound is the better of the two: the
			// spinner then sits in the space the list is about to occupy, and
			// the sheet does not jump when it arrives.
			LoadStateBox(
				state = state,
				onRetry = viewModel::refresh,
				modifier = Modifier.heightIn(max = LIST_HEIGHT),
			) { eq ->
				val listState = rememberLazyListState()
				// A device's own presets are appended after the two dozen
				// built-in ones, so the loaded one is routinely below the fold
				// when the sheet opens — and a selection nobody can see is
				// indistinguishable from no selection at all, which is exactly
				// how this read on a device with six custom presets.
				//
				// Only when it is out of sight, so tapping a preset never drags
				// the list under the finger. On first composition nothing is
				// laid out yet, which counts as out of sight and is what makes
				// the opening scroll happen.
				LaunchedEffect(eq.preset, eq.presets) {
					val index = eq.presets.indexOf(eq.preset)
					val seen = listState.layoutInfo.visibleItemsInfo.any { it.index == index }
					if (index >= 0 && !seen) listState.scrollToItem(index)
				}
				Column(modifier = Modifier.fillMaxHeight()) {
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
					// weight rather than nothing, so the list gets exactly what
					// the switch row left and scrolls inside it. Lazy rather
					// than a scrolling Column for the state alone — the list is
					// three dozen rows at most and needs no recycling, but
					// scrolling to an item needs an index, which a Column has no
					// notion of.
					LazyColumn(
						state = listState,
						modifier = Modifier.weight(1f),
					) {
						items(eq.presets) { preset ->
							PresetRow(
								name = preset,
								selected = preset == eq.preset,
								enabled = !busy,
								onClick = { viewModel.selectPreset(preset) },
							)
						}
					}
				}
			}
		}
	}
}

@Composable
private fun PresetRow(
	name: String,
	selected: Boolean,
	enabled: Boolean,
	onClick: () -> Unit,
) {
	Row(
		modifier = Modifier
			.fillMaxWidth()
			.clickable(enabled = enabled, onClick = onClick)
			.padding(horizontal = 24.dp, vertical = 10.dp),
		verticalAlignment = Alignment.CenterVertically,
		horizontalArrangement = Arrangement.spacedBy(16.dp),
	) {
		// onClick = null so the whole row is the target, the idiom the library
		// selector and the subtitle picker already use.
		RadioButton(selected = selected, onClick = null, enabled = enabled)
		Text(
			text = name,
			style = MaterialTheme.typography.bodyLarge,
			color = if (selected) {
				MaterialTheme.colorScheme.primary
			} else {
				MaterialTheme.colorScheme.onSurface
			},
		)
	}
}

/**
 * Tall enough for most of a preset list without covering the whole sheet, and
 * fixed so the loading state occupies the same space the list will.
 */
private val LIST_HEIGHT = 360.dp
