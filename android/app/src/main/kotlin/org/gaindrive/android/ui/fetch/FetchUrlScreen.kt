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
import androidx.compose.ui.text.input.KeyboardType
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.dp
import androidx.compose.ui.window.PopupProperties
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
			// Editable, and pre-filled when a share supplied it. Two entry points
			// share this screen — a share sheet, and the uploads listing's own
			// row, which opens it with nothing — and the second needs a field
			// here whatever the first would have preferred.
			OutlinedTextField(
				value = state.url,
				onValueChange = viewModel::onUrl,
				label = { Text("URL") },
				placeholder = { Text("https://…") },
				singleLine = true,
				enabled = !state.live && !state.submitting,
				keyboardOptions = KeyboardOptions(
					keyboardType = KeyboardType.Uri,
					imeAction = ImeAction.Next,
				),
				modifier = Modifier.fillMaxWidth(),
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

	// What the two fields are called. Not a destination — an admin picks that
	// when they promote this — only which words fit what is being filed.
	if (state.showKinds) {
		Row(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
			state.kinds.forEach { kind ->
				FilterChip(
					selected = kind == state.kind,
					onClick = { viewModel.onKind(kind) },
					enabled = !state.live && !state.submitting,
					label = { Text(kind.label) },
				)
			}
		}
	}

	// Completed against the whole library — every slice, every server — and
	// against staging, so a name typed a second time in a different spelling is
	// caught before it becomes a second artist.
	SuggestingField(
		value = state.artist,
		onValueChange = viewModel::onArtist,
		label = state.artistLabel,
		suggestions = state.suggestions,
		enabled = !state.live && !state.submitting,
		imeAction = ImeAction.Next,
	)

	OutlinedTextField(
		value = state.album,
		onValueChange = viewModel::onAlbum,
		label = { Text(state.albumLabel) },
		placeholder = { Text("From the title if left blank") },
		singleLine = true,
		enabled = !state.live && !state.submitting,
		keyboardOptions = KeyboardOptions(imeAction = ImeAction.Done),
		modifier = Modifier.fillMaxWidth(),
	)

	// Advisory and never blocking: nothing can collide here, since the batch is
	// a fresh directory and where it will eventually be promoted to is not
	// decided yet. Deliberately not coloured as an error for the same reason.
	state.existing?.let {
		Text(
			text = it,
			style = MaterialTheme.typography.bodyMedium,
			color = MaterialTheme.colorScheme.onSurface,
		)
	}

	// Two things people get wrong about this screen and cannot tell by looking:
	// that the names persist to the next fetch, and that nothing here reaches
	// the shared library on its own. "Uploads" reads like part of the library
	// and is not — only an admin can move something out of it.
	val handlers = state.target?.handlerNames.orEmpty().joinToString()
	Text(
		text = buildString {
			append("Goes to your own uploads; an admin moves it into the ")
			append("shared library. The names are kept for the next URL.")
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

/**
 * A text field that offers what the library already has, without restricting
 * what can be typed.
 *
 * A field rather than a picker, because most of what is filed here is new: the
 * suggestions exist to stop a *second spelling* of something already present,
 * not to constrain the answer. Each one says where it was found — a staging hit
 * is a different fact from a library hit, and only the second means the thing is
 * actually in the shared library.
 */
@Composable
private fun SuggestingField(
	value: String,
	onValueChange: (String) -> Unit,
	label: String,
	suggestions: List<NameSuggestion>,
	enabled: Boolean,
	imeAction: ImeAction,
) {
	var open by remember { mutableStateOf(false) }
	val matches = remember(value, suggestions) {
		if (value.isBlank()) emptyList()
		else suggestions
			.filter { it.name.contains(value, ignoreCase = true) && !it.name.equals(value, true) }
			.take(SUGGESTION_LIMIT)
	}

	Box {
		OutlinedTextField(
			value = value,
			onValueChange = {
				onValueChange(it)
				open = true
			},
			label = { Text(label) },
			placeholder = { Text("From the title if left blank") },
			singleLine = true,
			enabled = enabled,
			keyboardOptions = KeyboardOptions(imeAction = imeAction),
			modifier = Modifier.fillMaxWidth(),
		)
		DropdownMenu(
			expanded = open && matches.isNotEmpty(),
			onDismissRequest = { open = false },
			// Never takes focus: this hangs under a field still being typed in,
			// and a focusable popup would close the keyboard on every keystroke.
			properties = PopupProperties(focusable = false),
		) {
			matches.forEach { match ->
				DropdownMenuItem(
					text = { Text(match.name) },
					trailingIcon = {
						Text(
							text = match.where,
							style = MaterialTheme.typography.labelSmall,
							color = MaterialTheme.colorScheme.onSurfaceVariant,
						)
					},
					onClick = {
						open = false
						onValueChange(match.name)
					},
				)
			}
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

/** Enough to be useful, few enough not to bury the field being typed into. */
private const val SUGGESTION_LIMIT = 6
