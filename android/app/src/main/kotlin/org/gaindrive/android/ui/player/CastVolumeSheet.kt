package org.gaindrive.android.ui.player

import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.padding
import androidx.compose.material3.ExperimentalMaterial3Api
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.ModalBottomSheet
import androidx.compose.material3.Text
import androidx.compose.material3.rememberModalBottomSheetState
import androidx.compose.runtime.Composable
import androidx.compose.runtime.getValue
import androidx.compose.ui.Modifier
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.dp
import androidx.hilt.lifecycle.viewmodel.compose.hiltViewModel
import androidx.lifecycle.ViewModel
import androidx.lifecycle.compose.collectAsStateWithLifecycle
import dagger.hilt.android.lifecycle.HiltViewModel
import kotlinx.coroutines.flow.StateFlow
import org.gaindrive.android.playback.cast.CastDevice
import org.gaindrive.android.playback.cast.CastSession
import org.gaindrive.android.playback.cast.CastVolume
import javax.inject.Inject

/**
 * The receiver's device volume, over the Cast channel and never the WiiM's
 * HTTP API: SET_VOLUME is one code path for every receiver, and WiiMClient
 * stays off the casting path as WIIM.md prescribes.
 *
 * [CastSession] is injected directly and its device re-exported, the way
 * [WiiMControlsViewModel] does it and for its reason.
 */
@HiltViewModel
class CastVolumeViewModel @Inject constructor(
	private val castSession: CastSession,
) : ViewModel() {
	val device: StateFlow<CastDevice?> = castSession.device
	val volume: StateFlow<CastVolume?> = castSession.volume

	fun up() = step(CAST_VOLUME_STEP)
	fun down() = step(-CAST_VOLUME_STEP)

	// A no-op until the receiver has stated a level: stepping from a guess
	// could jump the volume, and the row's buttons are disabled anyway.
	private fun step(delta: Float) {
		val level = volume.value?.level ?: return
		castSession.setVolume(level + delta)
	}
}

/**
 * The volume row wired to the cast receiver. It resolves its own view model,
 * so a host sheet embeds it with no plumbing; [WiiMControlsSheet] and
 * [CastVolumeSheet] both do.
 */
@Composable
internal fun CastVolumeSection(viewModel: CastVolumeViewModel = hiltViewModel()) {
	val volume by viewModel.volume.collectAsStateWithLifecycle()
	VolumeRow(fraction = volume?.level, onDown = viewModel::down, onUp = viewModel::up)
}

/**
 * Speaker controls for a plain Chromecast: volume is the only control the
 * Cast protocol offers, so this is the whole sheet. A WiiM gets
 * [WiiMControlsSheet], whose private HTTP API reaches more.
 *
 * A second [ModalBottomSheet] beside the Now Playing one, hosted from
 * `GainDriveApp` exactly as [WiiMControlsSheet] is.
 */
@OptIn(ExperimentalMaterial3Api::class)
@Composable
fun CastVolumeSheet(
	onDismiss: () -> Unit,
	viewModel: CastVolumeViewModel = hiltViewModel(),
) {
	val device by viewModel.device.collectAsStateWithLifecycle()
	val sheetState = rememberModalBottomSheetState(skipPartiallyExpanded = true)

	ModalBottomSheet(onDismissRequest = onDismiss, sheetState = sheetState) {
		Column(modifier = Modifier.padding(bottom = 24.dp)) {
			Text(
				text = device?.name ?: "Cast device",
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
			CastVolumeSection(viewModel)
		}
	}
}

/**
 * A twentieth of the range per tap. The receiver's scale is 0..1 and its own
 * step size is unknowable, so this is a feel choice, not a mapping.
 */
private const val CAST_VOLUME_STEP = 0.05f
