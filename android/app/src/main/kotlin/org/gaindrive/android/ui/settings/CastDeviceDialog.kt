package org.gaindrive.android.ui.settings

import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.size
import androidx.compose.material3.AlertDialog
import androidx.compose.material3.CircularProgressIndicator
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.OutlinedButton
import androidx.compose.material3.OutlinedTextField
import androidx.compose.material3.Text
import androidx.compose.material3.TextButton
import androidx.compose.runtime.Composable
import androidx.compose.ui.Modifier
import androidx.compose.ui.text.input.ImeAction
import androidx.compose.ui.text.input.KeyboardType
import androidx.compose.foundation.text.KeyboardOptions
import androidx.compose.ui.unit.dp
import org.gaindrive.android.playback.cast.CastProbeResult

/**
 * Adds or edits one manually named Cast device.
 *
 * A dialog rather than a screen of its own: three fields, one of them usually
 * left at its default, opened from a list that is itself a section of a larger
 * screen. It carries the only "Test" in the feature - a row is opened by
 * tapping it, so testing a saved device costs the same one tap either way, and
 * one test surface means one piece of in-flight state to reason about.
 */
@Composable
fun CastDeviceDialog(
	draft: CastDeviceDraft,
	onAddress: (String) -> Unit,
	onName: (String) -> Unit,
	onPort: (String) -> Unit,
	onTest: () -> Unit,
	onSave: () -> Unit,
	onDismiss: () -> Unit,
) {
	AlertDialog(
		onDismissRequest = onDismiss,
		title = { Text(if (draft.isNew) "Add device" else "Edit device") },
		text = {
			Column(verticalArrangement = Arrangement.spacedBy(12.dp)) {
				OutlinedTextField(
					value = draft.address,
					onValueChange = onAddress,
					label = { Text("Address") },
					placeholder = { Text("192.168.1.42") },
					singleLine = true,
					keyboardOptions = KeyboardOptions(
						keyboardType = KeyboardType.Uri,
						imeAction = ImeAction.Next,
					),
					modifier = Modifier.fillMaxWidth(),
				)

				OutlinedTextField(
					value = draft.name,
					onValueChange = onName,
					label = { Text("Name") },
					supportingText = { Text("Optional; defaults to the address") },
					singleLine = true,
					keyboardOptions = KeyboardOptions(imeAction = ImeAction.Next),
					modifier = Modifier.fillMaxWidth(),
				)

				OutlinedTextField(
					value = draft.port,
					onValueChange = onPort,
					label = { Text("Port") },
					// Named rather than left bare, because 8009 is the only value
					// anyone should ever need and a changed one wants explaining.
					supportingText = { Text("8009 unless you know otherwise") },
					singleLine = true,
					keyboardOptions = KeyboardOptions(
						keyboardType = KeyboardType.Number,
						imeAction = ImeAction.Done,
					),
					modifier = Modifier.fillMaxWidth(),
				)

				OutlinedButton(
					enabled = !draft.testing && draft.canSave,
					onClick = onTest,
				) {
					if (draft.testing) {
						CircularProgressIndicator(
							modifier = Modifier.size(16.dp),
							strokeWidth = 2.dp,
						)
					} else {
						Text("Test")
					}
				}

				draft.testResult?.let { ProbeResult(it) }
			}
		},
		confirmButton = {
			TextButton(enabled = draft.canSave, onClick = onSave) { Text("Save") }
		},
		dismissButton = {
			TextButton(onClick = onDismiss) { Text("Cancel") }
		},
	)
}

/**
 * The three outcomes, in the three colours they deserve.
 *
 * [CastProbeResult.Silent] is not an error colour on purpose: the address is
 * reachable and something is listening, which is a different fix from a wrong
 * address and is worth not shouting about.
 */
@Composable
private fun ProbeResult(result: CastProbeResult) {
	val (text, colour) = when (result) {
		is CastProbeResult.Answered -> {
			val showing = result.runningApp?.let { " It is showing $it." }.orEmpty()
			"The device answered.$showing" to MaterialTheme.colorScheme.primary
		}

		CastProbeResult.Silent ->
			"Something answered on that port but did not speak Cast. Check the " +
				"address is the television and not another machine." to
				MaterialTheme.colorScheme.onSurfaceVariant

		CastProbeResult.Unreachable ->
			"Could not reach that address. Check the device is on, and that this " +
				"phone is on the same network as it." to MaterialTheme.colorScheme.error
	}
	Text(text = text, color = colour, style = MaterialTheme.typography.bodyMedium)
}
