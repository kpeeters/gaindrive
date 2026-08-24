package org.gaindrive.android.ui.fetch

import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.text.KeyboardOptions
import androidx.compose.foundation.verticalScroll
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.automirrored.filled.ArrowBack
import androidx.compose.material.icons.filled.ArrowDropDown
import androidx.compose.material3.Button
import androidx.compose.material3.CircularProgressIndicator
import androidx.compose.material3.DropdownMenu
import androidx.compose.material3.DropdownMenuItem
import androidx.compose.material3.ExperimentalMaterial3Api
import androidx.compose.material3.FilterChip
import androidx.compose.material3.HorizontalDivider
import androidx.compose.material3.Icon
import androidx.compose.material3.IconButton
import androidx.compose.material3.LinearProgressIndicator
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.OutlinedButton
import androidx.compose.material3.OutlinedTextField
import androidx.compose.material3.RadioButton
import androidx.compose.material3.Scaffold
import androidx.compose.material3.Text
import androidx.compose.material3.TextButton
import androidx.compose.material3.TopAppBar
import androidx.compose.runtime.Composable
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.text.input.ImeAction
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.dp
import androidx.hilt.navigation.compose.hiltViewModel
import androidx.lifecycle.compose.collectAsStateWithLifecycle
import org.gaindrive.android.data.model.ServerId
import org.gaindrive.android.ui.LocalAvailability

/**
 * What a URL shared with the app opens onto: the link, somewhere to put it, and
 * then the fetch running.
 *
 * The panel stays on screen while the job runs rather than closing behind a
 * confirmation. A fetch is a download and a scan on the far side, so it can take
 * minutes, and this is the only place that reports on it — the app has no
 * notification for one, and the shared URL has by then been consumed.
 *
 * It does not own the job. Leaving stops the polling, not the fetch.
 */
@OptIn(ExperimentalMaterial3Api::class)
@Composable
fun FetchUrlScreen(
	onDone: () -> Unit,
	viewModel: FetchUrlViewModel = hiltViewModel(),
) {
	val state by viewModel.state.collectAsStateWithLifecycle()
	val online = LocalAvailability.current.online

	Scaffold(
		topBar = {
			TopAppBar(
				title = { Text("Fetch to library") },
				navigationIcon = {
					IconButton(onClick = onDone) {
						Icon(Icons.AutoMirrored.Filled.ArrowBack, contentDescription = "Back")
					}
				},
			)
		},
	) { insets ->
		Column(
			modifier = Modifier
				.fillMaxSize()
				.padding(insets)
				.padding(16.dp)
				// The keyboard plus a running job is taller than a phone, and
				// the job is the part that must not be pushed out of reach.
				.verticalScroll(rememberScrollState()),
			verticalArrangement = Arrangement.spacedBy(12.dp),
		) {
			Text(
				text = state.url,
				style = MaterialTheme.typography.bodyMedium,
				color = MaterialTheme.colorScheme.onSurfaceVariant,
				maxLines = 2,
				overflow = TextOverflow.Ellipsis,
			)

			when {
				// Only the probe's own outcome is reported here. Being offline is
				// already said by the shell's banner, but it is also the reason
				// the list below is empty, so it has to be said again in terms of
				// what cannot be done.
				!online -> Note(
					"You are offline. Nothing can be fetched until you have a connection.",
					error = true,
				)

				state.targets == null -> Row(
					horizontalArrangement = Arrangement.spacedBy(12.dp),
					verticalAlignment = Alignment.CenterVertically,
				) {
					CircularProgressIndicator(
						modifier = Modifier.size(16.dp),
						strokeWidth = 2.dp,
					)
					Text("Looking for a server that can fetch this…")
				}

				state.targets.orEmpty().isEmpty() -> Note(
					"No configured server can fetch a URL. It needs a gaindrive server " +
						"with a fetch handler set up, and an account allowed to upload.",
					error = true,
				)

				else -> {
					FetchForm(state, viewModel)
					state.job?.let {
						HorizontalDivider()
						JobPanel(state, onCancel = viewModel::cancel)
					}
				}
			}

			state.error?.let { Note(it, error = true) }
		}
	}
}

@Composable
private fun FetchForm(state: FetchUrlUiState, viewModel: FetchUrlViewModel) {
	val targets = state.targets.orEmpty()

	// Hidden with one server, like LibrarySelector: a picker with a single
	// entry is a control that does nothing, and the common case should not pay
	// for the general one.
	if (targets.size > 1) {
		ServerPicker(
			targets = targets,
			chosen = state.server,
			enabled = !state.live && !state.submitting,
			onSelect = viewModel::onServer,
		)
	}

	OutlinedTextField(
		value = state.artist,
		onValueChange = viewModel::onArtist,
		label = { Text("Artist") },
		placeholder = { Text("From the title if left blank") },
		singleLine = true,
		enabled = !state.live && !state.submitting,
		keyboardOptions = KeyboardOptions(imeAction = ImeAction.Next),
		modifier = Modifier.fillMaxWidth(),
	)

	OutlinedTextField(
		value = state.album,
		onValueChange = viewModel::onAlbum,
		label = { Text("Album") },
		placeholder = { Text("From the title if left blank") },
		singleLine = true,
		enabled = !state.live && !state.submitting,
		keyboardOptions = KeyboardOptions(imeAction = ImeAction.Done),
		modifier = Modifier.fillMaxWidth(),
	)

	// Says that the names are kept, because a field that remembers itself is a
	// surprise otherwise — the next share opens with them already filled in.
	val handlers = state.target?.handlerNames.orEmpty().joinToString()
	Text(
		text = buildString {
			append("Kept for the next URL you share.")
			if (handlers.isNotBlank()) append(" Handled by $handlers.")
		},
		style = MaterialTheme.typography.bodySmall,
		color = MaterialTheme.colorScheme.onSurfaceVariant,
	)

	if (state.showModes) {
		Row(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
			FilterChip(
				selected = state.audio,
				onClick = { viewModel.onAudio(true) },
				enabled = !state.live && !state.submitting,
				label = { Text("Audio") },
			)
			FilterChip(
				selected = !state.audio,
				onClick = { viewModel.onAudio(false) },
				enabled = !state.live && !state.submitting,
				label = { Text("Video") },
			)
		}
	}

	Row(
		horizontalArrangement = Arrangement.spacedBy(12.dp),
		verticalAlignment = Alignment.CenterVertically,
	) {
		Button(enabled = state.canSubmit, onClick = viewModel::submit) {
			if (state.submitting) {
				CircularProgressIndicator(
					modifier = Modifier.size(16.dp),
					strokeWidth = 2.dp,
				)
			} else {
				Text("Fetch")
			}
		}
		TextButton(
			enabled = state.artist.isNotEmpty() || state.album.isNotEmpty(),
			onClick = viewModel::clearNames,
		) {
			Text("Clear names")
		}
	}
}

@Composable
private fun ServerPicker(
	targets: List<FetchTarget>,
	chosen: ServerId?,
	enabled: Boolean,
	onSelect: (ServerId) -> Unit,
) {
	var open by remember { mutableStateOf(false) }
	val name = targets.firstOrNull { it.id == chosen }?.name ?: "Choose a server"

	Box {
		OutlinedButton(enabled = enabled, onClick = { open = true }) {
			Text(name)
			Icon(Icons.Default.ArrowDropDown, contentDescription = null)
		}
		DropdownMenu(expanded = open, onDismissRequest = { open = false }) {
			targets.forEach { target ->
				DropdownMenuItem(
					text = { Text(target.name) },
					leadingIcon = {
						RadioButton(selected = target.id == chosen, onClick = null)
					},
					onClick = {
						open = false
						onSelect(target.id)
					},
				)
			}
		}
	}
}

/** The running job: where it has got to, and the way to stop it. */
@Composable
private fun JobPanel(state: FetchUrlUiState, onCancel: () -> Unit) {
	val job = state.job ?: return

	Column(verticalArrangement = Arrangement.spacedBy(8.dp)) {
		Row(
			modifier = Modifier.fillMaxWidth(),
			horizontalArrangement = Arrangement.SpaceBetween,
			verticalAlignment = Alignment.CenterVertically,
		) {
			Text(
				text = when (state.state) {
					FetchState.QUEUED -> "Queued"
					FetchState.RUNNING -> "Downloading"
					// Named for what the user is waiting for rather than for
					// what the server is doing: the fetch is not finished until
					// the library knows about it, which is the whole reason the
					// server reports this state separately.
					FetchState.SCANNING -> "Adding to the library"
					FetchState.DONE -> "Done — ${job.files} file(s)"
					FetchState.ERROR -> "Failed"
					FetchState.CANCELLED -> "Cancelled"
					// A state this build predates. Shown raw rather than hidden:
					// the server's own word for it is more use than silence.
					FetchState.UNKNOWN -> job.state.ifBlank { "Working" }
				},
				style = MaterialTheme.typography.titleSmall,
			)
			if (state.state.isCancellable) {
				TextButton(onClick = onCancel) { Text("Cancel") }
			}
		}

		if (state.state == FetchState.RUNNING || state.state == FetchState.SCANNING) {
			LinearProgressIndicator(
				progress = { job.percent.coerceIn(0, 100) / 100f },
				modifier = Modifier.fillMaxWidth(),
			)
		}

		// The tool's own last line. One line, ellipsised: it changes on every
		// poll, and letting it wrap makes the whole panel jump every two
		// seconds.
		if (job.detail.isNotBlank()) {
			Text(
				text = job.detail,
				style = MaterialTheme.typography.bodySmall,
				color = MaterialTheme.colorScheme.onSurfaceVariant,
				maxLines = 1,
				overflow = TextOverflow.Ellipsis,
			)
		}

		if (job.error.isNotBlank()) Note(job.error, error = true)

		if (state.state == FetchState.DONE) {
			// The names the *server* recorded, which are the typed ones only when
			// they were typed — otherwise the handler took them from the title
			// and the phone never saw them.
			val artist = job.artist.ifBlank { "the title's artist" }
			val album = job.album.ifBlank { "the title's album" }
			Note("In your uploads, under $artist · $album.")
		}
	}
}

@Composable
private fun Note(text: String, error: Boolean = false) {
	Text(
		text = text,
		style = MaterialTheme.typography.bodyMedium,
		color = if (error) MaterialTheme.colorScheme.error
		else MaterialTheme.colorScheme.onSurfaceVariant,
	)
}
