package org.gaindrive.android.ui.components

import android.widget.Toast
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.filled.Download
import androidx.compose.material.icons.filled.DownloadDone
import androidx.compose.material3.Icon
import androidx.compose.material3.IconButton
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.getValue
import androidx.compose.ui.platform.LocalContext
import androidx.hilt.navigation.compose.hiltViewModel
import androidx.lifecycle.compose.collectAsStateWithLifecycle
import org.gaindrive.android.data.cache.PinKind
import org.gaindrive.android.data.model.ItemRef
import org.gaindrive.android.ui.PinViewModel

/**
 * The download toggle for an album or a playlist, for a top app bar.
 *
 * Refusals go out as a toast rather than a snackbar: there is no snackbar host
 * in this app, and a message that has to outlive the sheet or bar it was raised
 * from is exactly what a toast is for.
 */
@Composable
fun PinAction(
	ref: ItemRef,
	kind: PinKind,
	viewModel: PinViewModel = hiltViewModel(),
) {
	val pinned by viewModel.pinnedRefs.collectAsStateWithLifecycle()
	val message by viewModel.message.collectAsStateWithLifecycle()
	val context = LocalContext.current

	LaunchedEffect(message) {
		message?.let {
			Toast.makeText(context, it, Toast.LENGTH_LONG).show()
			viewModel.consumeMessage()
		}
	}

	val isPinned = ref.encode() in pinned
	IconButton(onClick = { viewModel.toggle(ref, kind) }) {
		Icon(
			imageVector = if (isPinned) Icons.Default.DownloadDone else Icons.Default.Download,
			contentDescription = if (isPinned) "Remove download" else "Download",
		)
	}
}
