package org.gaindrive.android.ui.components

import android.widget.Toast
import androidx.compose.animation.core.animateFloatAsState
import androidx.compose.foundation.layout.size
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.filled.Download
import androidx.compose.material.icons.filled.DownloadDone
import androidx.compose.material.icons.filled.ErrorOutline
import androidx.compose.material.icons.filled.Schedule
import androidx.compose.material3.CircularProgressIndicator
import androidx.compose.material3.Icon
import androidx.compose.material3.IconButton
import androidx.compose.material3.MaterialTheme
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.getValue
import androidx.compose.ui.Modifier
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.unit.dp
import androidx.hilt.navigation.compose.hiltViewModel
import androidx.lifecycle.compose.collectAsStateWithLifecycle
import org.gaindrive.android.data.cache.PinKind
import org.gaindrive.android.data.cache.PinPhase
import org.gaindrive.android.data.cache.PinStatus
import org.gaindrive.android.data.model.ItemRef
import org.gaindrive.android.ui.PinViewModel

/**
 * The download control for an album or a playlist, for a top app bar.
 *
 * Three states, because "I tapped it and nothing visibly changed" is the
 * complaint a plain toggle earns: nothing yet, arriving, here. The middle one
 * is a ring showing how many of the tracks have landed — progress is counted in
 * whole tracks, since that is the granularity both Media3 and the cache report.
 *
 * Refusals go out as a toast rather than a snackbar: there is no snackbar host
 * in this app, and a message that has to outlive the bar it was raised from is
 * exactly what a toast is for.
 */
@Composable
fun PinAction(
	ref: ItemRef,
	kind: PinKind,
	viewModel: PinViewModel = hiltViewModel(),
) {
	val statuses by viewModel.statuses.collectAsStateWithLifecycle()
	val message by viewModel.message.collectAsStateWithLifecycle()
	val context = LocalContext.current

	LaunchedEffect(message) {
		message?.let {
			Toast.makeText(context, it, Toast.LENGTH_LONG).show()
			viewModel.consumeMessage()
		}
	}

	val status = statuses[ref.encode()]
	IconButton(onClick = { viewModel.toggle(ref, kind) }) {
		DownloadIndicator(status)
	}
}

/**
 * Shared by the app-bar button and the track sheet, so "downloading" looks the
 * same wherever it is reported.
 */
@Composable
fun DownloadIndicator(status: PinStatus?, modifier: Modifier = Modifier) {
	if (status == null) {
		Icon(
			imageVector = Icons.Default.Download,
			contentDescription = "Download",
			modifier = modifier,
		)
		return
	}

	when (status.phase) {
		PinPhase.COMPLETE -> Icon(
			imageVector = Icons.Default.DownloadDone,
			contentDescription = "Downloaded — tap to remove",
			modifier = modifier,
		)

		// Distinct from running, because the fix is the user's: they are on a
		// metered connection with "download on Wi-Fi only" set, and a ring
		// sitting at zero would look like a broken download rather than an
		// obedient one.
		PinPhase.WAITING -> Icon(
			imageVector = Icons.Default.Schedule,
			contentDescription = "Waiting for Wi-Fi — tap to remove",
			tint = MaterialTheme.colorScheme.onSurfaceVariant,
			modifier = modifier,
		)

		PinPhase.FAILED -> Icon(
			imageVector = Icons.Default.ErrorOutline,
			contentDescription = "Download failed — tap to try again",
			tint = MaterialTheme.colorScheme.error,
			modifier = modifier,
		)

		PinPhase.RUNNING -> {
			// Animated, because progress arrives a whole track at a time and a
			// ring that jumps in twelfths reads as broken rather than busy.
			val fraction by animateFloatAsState(
				targetValue = status.fraction,
				label = "downloadProgress",
			)
			CircularProgressIndicator(
				progress = { fraction },
				strokeWidth = 2.dp,
				color = MaterialTheme.colorScheme.primary,
				// Matches an icon's own footprint so the bar does not reflow
				// when the state changes.
				modifier = modifier.size(20.dp),
			)
		}
	}
}

/** What to call the action, given the same status the indicator draws. */
fun downloadActionLabel(status: PinStatus?): String = when (status?.phase) {
	null -> "Download"
	PinPhase.COMPLETE -> "Remove download"
	PinPhase.WAITING -> "Waiting for Wi-Fi — remove"
	PinPhase.FAILED -> "Download failed — try again"
	// Says what tapping does, not what is happening — the ring already says
	// that, and "Downloading…" as a menu item invites a tap that then cancels.
	PinPhase.RUNNING -> "Cancel download (${status.stored} of ${status.total})"
}
