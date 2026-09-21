package org.gaindrive.android.ui.player

import android.content.BroadcastReceiver
import android.content.Context
import android.content.Intent
import android.content.IntentFilter
import android.media.AudioManager
import android.util.Log
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
import androidx.compose.runtime.DisposableEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.unit.dp
import androidx.core.content.ContextCompat
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
 * [CastVolumeSheet], volume being all the Cast protocol offers.
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
		Column(modifier = Modifier.padding(bottom = 24.dp)) {
			// A stated height for the same reason WiiMControlsSheet states
			// one: LoadStateBox fills what it is given, and a sheet's Column
			// gives an unbounded nothing. Fixed rather than a bound, so the
			// spinner and the unavailable message occupy the space the panel
			// will.
			LoadStateBox(
				state = state,
				modifier = Modifier.heightIn(max = PANEL_HEIGHT),
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
			// Below the box rather than in it: the media volume is there whether
			// or not the equalizer effect could be created.
			val volume = rememberMusicVolume()
			VolumeRow(fraction = volume.fraction, onDown = volume::down, onUp = volume::up)
		}
	}
}

/**
 * STREAM_MUSIC as the rocker sees it: the buttons step by one like the rocker
 * does, the indicator reads the level back as a fraction.
 */
@Composable
private fun rememberMusicVolume(): MusicVolume {
	val context = LocalContext.current
	val volume = remember {
		MusicVolume(context.getSystemService(Context.AUDIO_SERVICE) as? AudioManager)
	}
	// The rocker can move the level while the sheet is open, and nothing
	// public announces that: VOLUME_CHANGED_ACTION is a system broadcast the
	// SDK offers no constant for, so the literal is all there is.
	DisposableEffect(Unit) {
		val receiver = object : BroadcastReceiver() {
			override fun onReceive(context: Context, intent: Intent) = volume.reread()
		}
		ContextCompat.registerReceiver(
			context,
			receiver,
			IntentFilter("android.media.VOLUME_CHANGED_ACTION"),
			ContextCompat.RECEIVER_NOT_EXPORTED,
		)
		onDispose { context.unregisterReceiver(receiver) }
	}
	return volume
}

private class MusicVolume(private val audio: AudioManager?) {
	var fraction by mutableStateOf(read())
		private set

	fun up() = adjust(AudioManager.ADJUST_RAISE)
	fun down() = adjust(AudioManager.ADJUST_LOWER)
	fun reread() { fraction = read() }

	// No FLAG_SHOW_UI for VideoGestures' reason: the sheet has its own
	// indicator, and the system panel would land on top of it.
	private fun adjust(direction: Int) {
		try {
			audio?.adjustStreamVolume(AudioManager.STREAM_MUSIC, direction, 0)
		} catch (e: SecurityException) {
			// Do Not Disturb can refuse the change.
			Log.w(TAG, "volume change refused", e)
		}
		reread()
	}

	private fun read(): Float? {
		val audio = audio ?: return null
		val max = audio.getStreamMaxVolume(AudioManager.STREAM_MUSIC)
		if (max <= 0) return null
		return (audio.getStreamVolume(AudioManager.STREAM_MUSIC).toFloat() / max)
			.coerceIn(0f, 1f)
	}
}

private const val TAG = "GainDriveEq"

private fun freqLabel(hz: Int): String = when {
	hz < 1000 -> "$hz Hz"
	hz % 1000 == 0 -> "${hz / 1000} kHz"
	else -> "%.1f kHz".format(hz / 1000.0)
}

/** The switch row, the faders with their labels, the preset row, and the
 * name field the save button reveals under it. */
private val PANEL_HEIGHT = 440.dp
