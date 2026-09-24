package org.gaindrive.android.ui.settings

import androidx.activity.compose.BackHandler
import androidx.activity.compose.LocalActivity
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.layout.widthIn
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.automirrored.filled.ArrowBack
import androidx.compose.material3.Button
import androidx.compose.material3.CircularProgressIndicator
import androidx.compose.material3.ExperimentalMaterial3Api
import androidx.compose.material3.Icon
import androidx.compose.material3.IconButton
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.OutlinedButton
import androidx.compose.material3.OutlinedTextField
import androidx.compose.material3.Scaffold
import androidx.compose.material3.Text
import androidx.compose.material3.TopAppBar
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.getValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.text.input.ImeAction
import androidx.compose.ui.text.input.KeyboardType
import androidx.compose.ui.text.input.PasswordVisualTransformation
import androidx.compose.foundation.text.KeyboardOptions
import androidx.compose.ui.unit.dp
import androidx.hilt.lifecycle.viewmodel.compose.hiltViewModel
import androidx.lifecycle.compose.collectAsStateWithLifecycle
import org.gaindrive.android.net.ConnectionTest
import org.gaindrive.android.ui.LocalIsTv

/**
 * Adding a server and editing one are the same screen. That is deliberate:
 * with several servers there is no "log in" moment to build a separate screen
 * around, and a first-run screen would drift from the one used later.
 */
@OptIn(ExperimentalMaterial3Api::class)
@Composable
fun ServerEditScreen(
	onDone: () -> Unit,
	viewModel: ServerEditViewModel = hiltViewModel(),
) {
	val state by viewModel.state.collectAsStateWithLifecycle()
	// The TV form is trimmed to fit a 540dp screen whole: the column has no
	// scroll, so anything below the fold - the test result above all - would
	// simply be invisible there.
	val isTv = LocalIsTv.current

	LaunchedEffect(state.saved) {
		if (state.saved) onDone()
	}

	// The first-run copy has nothing behind it but an empty server list, so
	// popping it would be a dead end dressed as an exit; the arrow is not
	// drawn. The remote's BACK leaves the app instead: swallowing it fails
	// Play's TV review (TV-DB), which wants BACK to reach the home screen.
	// Saving still leaves via onDone.
	val activity = LocalActivity.current
	BackHandler(enabled = state.firstRun) { activity?.finish() }

	Scaffold(
		topBar = {
			TopAppBar(
				title = { Text(if (state.isNew) "Add server" else "Edit server") },
				navigationIcon = {
					if (!state.firstRun) {
						IconButton(onClick = onDone) {
							Icon(Icons.AutoMirrored.Filled.ArrowBack, contentDescription = "Back")
						}
					}
				},
			)
		},
	) { insets ->
		// The pairing pane rides on the TV form only: pairing exists to spare
		// the d-pad typing, and an edit of an existing server has nothing to
		// receive. The form shifts from centered to leading to make the room.
		val paired = isTv && state.isNew
		Box(
			modifier = Modifier.fillMaxSize().padding(insets),
			contentAlignment = if (paired) Alignment.TopStart else Alignment.TopCenter,
		) {
			Column(
				modifier = Modifier
					.widthIn(max = FORM_MAX_WIDTH)
					.fillMaxWidth()
					.padding(16.dp),
				verticalArrangement = Arrangement.spacedBy(12.dp),
			) {
				OutlinedTextField(
					value = state.url,
					onValueChange = viewModel::onUrl,
					label = { Text("Server URL") },
					placeholder = { Text("https://music.example.com") },
					singleLine = true,
					keyboardOptions = KeyboardOptions(
						keyboardType = KeyboardType.Uri,
						imeAction = ImeAction.Next,
					),
					modifier = Modifier.fillMaxWidth(),
				)

				OutlinedTextField(
					value = state.username,
					onValueChange = viewModel::onUsername,
					label = { Text("Username") },
					singleLine = true,
					keyboardOptions = KeyboardOptions(imeAction = ImeAction.Next),
					modifier = Modifier.fillMaxWidth(),
				)

				// Typed explicitly: an `if (…) { { … } } else null` leaves the
				// compiler guessing whether the braces are a block or a lambda.
				val passwordHint: (@Composable () -> Unit)? =
					if (state.isNew) null
					else {
						{ Text("Leave blank to keep the current password") }
					}

				OutlinedTextField(
					value = state.password,
					onValueChange = viewModel::onPassword,
					label = { Text("Password") },
					supportingText = passwordHint,
					singleLine = true,
					visualTransformation = PasswordVisualTransformation(),
					keyboardOptions = KeyboardOptions(
						keyboardType = KeyboardType.Password,
						imeAction = ImeAction.Done,
					),
					modifier = Modifier.fillMaxWidth(),
				)

				OutlinedTextField(
					value = state.name,
					onValueChange = viewModel::onName,
					label = { Text("Display name") },
					supportingText = { Text("Optional; defaults to the host name") },
					singleLine = true,
					modifier = Modifier.fillMaxWidth(),
				)

				// The subtitle carries the consequence because saving applies it
				// with no confirmation of its own, and this server's stored library
				// goes when it changes.
				//
				// Not on TV, for space and for sense: the tall subtitle is what
				// pushes the test result off a 540dp screen, and folder browsing
				// is a curation concern nobody settles from a sofa. A server
				// edited on TV keeps whatever the setting already is.
				if (!isTv) {
					SwitchRow(
						title = "Browse by folder",
						subtitle = "Uses the server's folders instead of its tags. Turn this " +
							"on for a library whose tags are patchy. Changing it reloads " +
							"this server's library; downloads are kept.",
						checked = state.browseByFolder,
						onCheckedChange = viewModel::onBrowseByFolder,
					)
				}

				Row(
					horizontalArrangement = Arrangement.spacedBy(12.dp),
					verticalAlignment = Alignment.CenterVertically,
				) {
					OutlinedButton(
						// Testing an edit with a blank password would test the
						// wrong thing, so it needs one typed in either way.
						enabled = !state.testing && state.url.isNotBlank() &&
							state.username.isNotBlank() && state.password.isNotBlank(),
						onClick = viewModel::test,
					) {
						if (state.testing) {
							CircularProgressIndicator(
								modifier = Modifier.size(16.dp),
								strokeWidth = 2.dp,
							)
						} else {
							Text("Test connection")
						}
					}

					Button(
						enabled = state.canSave,
						onClick = viewModel::save,
					) {
						Text("Save")
					}

					// Beside the buttons on TV rather than under them, so the
					// verdict is on screen the moment it lands; below them
					// otherwise, where a phone has the height and not the width.
					if (isTv) {
						state.testResult?.let {
							Box(modifier = Modifier.weight(1f)) { TestResult(it) }
						}
					}
				}

				if (!isTv) state.testResult?.let { TestResult(it) }
			}

			if (paired) {
				PairingPane(
					// Imported servers land exactly where a saved form goes.
					onImported = onDone,
					modifier = Modifier
						.align(Alignment.TopEnd)
						.padding(top = 16.dp, end = 24.dp),
				)
			}
		}
	}
}

@Composable
private fun TestResult(result: ConnectionTest) {
	val (text, colour) = when (result) {
		is ConnectionTest.Reachable ->
			"Connected." to MaterialTheme.colorScheme.primary
		is ConnectionTest.Rejected ->
			"The server rejected that: ${result.message}" to MaterialTheme.colorScheme.error
		is ConnectionTest.Unreachable ->
			"Could not reach the server: ${result.message}" to MaterialTheme.colorScheme.error
		is ConnectionTest.Unverified ->
			// Deliberately not the success colour: nothing was disproved, but
			// nothing was proved either.
			"The server answered, but did not send its library in time. " +
				"Saving is fine; the artist list may just be slow." to
				MaterialTheme.colorScheme.onSurfaceVariant
	}
	Text(text = text, color = colour, style = MaterialTheme.typography.bodyMedium)
}

/**
 * A full-window form is the one place left where the content is not already
 * bounded by a pane, and a field stretched across 1200dp is the classic
 * stretched-phone-app tell. About the measure a paragraph is comfortable at,
 * and the same figure Material 3 caps a bottom sheet with.
 */
private val FORM_MAX_WIDTH = 640.dp
