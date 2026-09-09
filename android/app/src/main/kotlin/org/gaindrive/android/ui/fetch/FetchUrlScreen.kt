package org.gaindrive.android.ui.fetch

import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.layout.widthIn
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
import org.gaindrive.android.data.model.FetchState
import org.gaindrive.android.data.model.ServerId
import org.gaindrive.android.net.FetchJobDto
import org.gaindrive.android.ui.LocalAvailability
import org.gaindrive.android.ui.components.relativeTime

/**
 * What a URL shared with the app opens onto: the link, somewhere to put it, and
 * then the fetch running.
 *
 * The panel stays on screen while the job runs rather than closing behind a
 * confirmation. A fetch is a download and a scan on the far side, so it can take
 * minutes, and the shared URL has by then been consumed.
 *
 * **It does not own the job — `FetchMonitor` does, and leaving now stops
 * nothing.** This used to run the poll itself, in its own `viewModelScope`, on a
 * route that is a drill-down: leaving cancelled the poll and coming back built a
 * fresh view model with no job in it, so a fetch still downloading looked
 * exactly like a first visit and pasting the same URL again was the obvious next
 * move. The panel now adopts whatever the account already has running, and the
 * shell's own strip reports it from every other screen.
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
		Box(
			// The cap and the centring are on the inner column; the scroll is
			// too, so the gesture follows the content rather than the empty
			// margins beside it.
			modifier = Modifier.fillMaxSize().padding(insets),
			contentAlignment = Alignment.TopCenter,
		) {
			Column(
				modifier = Modifier
					.widthIn(max = FORM_MAX_WIDTH)
					.fillMaxWidth()
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
					enabled = !state.submitting,
					keyboardOptions = KeyboardOptions(
						keyboardType = KeyboardType.Uri,
						imeAction = ImeAction.Next,
					),
					modifier = Modifier.fillMaxWidth(),
				)

				// Directly under the URL, not down with the names' own note.
				// That one answers a question about the two name fields and sits
				// under them; this answers a question about the URL — and unlike
				// that one it can disable the button, so it has to be beside the
				// thing it disables rather than stacked into a paragraph the
				// reader has to sort through.
				duplicateNote(state.duplicate)?.let { Note(it) }

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
						if (state.jobs.isNotEmpty()) {
							HorizontalDivider()
							JobList(state, onCancel = viewModel::cancel)
						}
					}
				}

				state.error?.let { Note(it, error = true) }
			}
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
			enabled = !state.submitting,
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
					enabled = !state.submitting,
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
		enabled = !state.submitting,
		imeAction = ImeAction.Next,
	)

	OutlinedTextField(
		value = state.album,
		onValueChange = viewModel::onAlbum,
		label = { Text(state.albumLabel) },
		placeholder = { Text("From the title if left blank") },
		singleLine = true,
		enabled = !state.submitting,
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
				enabled = !state.submitting,
				label = { Text("Audio") },
			)
			FilterChip(
				selected = !state.audio,
				onClick = { viewModel.onAudio(false) },
				enabled = !state.submitting,
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

/**
 * Every job the chosen server is reporting, newest first.
 *
 * A list rather than the single panel this replaced, because the screen adopts
 * whatever the account already has running — including a fetch begun in another
 * client — and a second one would otherwise be invisible. The web client's own
 * bar has always drawn a list, for the same reason.
 */
@Composable
private fun JobList(state: FetchUrlUiState, onCancel: (String) -> Unit) {
	Column(verticalArrangement = Arrangement.spacedBy(12.dp)) {
		if (state.contactLost) {
			Note(
				"Lost contact with the server. A fetch below may still be running; " +
					"what it says is the last thing we heard.",
				error = true,
			)
		}
		state.jobs.forEach { job ->
			JobRow(job, onCancel = { onCancel(job.id) })
		}
	}
}

/** One job: where it has got to, and the way to stop it. */
@Composable
private fun JobRow(job: FetchJobDto, onCancel: () -> Unit) {
	val state = FetchState.of(job.state)

	Column(verticalArrangement = Arrangement.spacedBy(8.dp)) {
		Row(
			modifier = Modifier.fillMaxWidth(),
			horizontalArrangement = Arrangement.SpaceBetween,
			verticalAlignment = Alignment.CenterVertically,
		) {
			Text(
				text = when (state) {
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
			if (state.isCancellable) {
				TextButton(onClick = onCancel) { Text("Cancel") }
			}
		}

		// What it is, so several rows are told apart — and so a job this panel
		// did not start says what it is rather than only how far along it is.
		Text(
			text = jobLabel(job),
			style = MaterialTheme.typography.bodySmall,
			color = MaterialTheme.colorScheme.onSurfaceVariant,
			maxLines = 1,
			overflow = TextOverflow.Ellipsis,
		)

		if (state == FetchState.RUNNING || state == FetchState.SCANNING) {
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

		if (state == FetchState.DONE) {
			// The names the *server* recorded, which are the typed ones only when
			// they were typed — otherwise the handler took them from the title
			// and the phone never saw them.
			val artist = job.artist.ifBlank { "the title's artist" }
			val album = job.album.ifBlank { "the title's album" }
			Note("In your uploads, under $artist · $album.")
		}
	}
}

/**
 * How a job is named when it is one of several: the typed names when there were
 * any, otherwise the tool and the mode. The web client's row says the same
 * thing, and a queued job has nothing else to identify it by.
 */
private fun jobLabel(job: FetchJobDto): String =
	if (job.artist.isNotBlank() || job.album.isNotBlank())
		"${job.artist.ifBlank { "…" }} · ${job.album.ifBlank { "…" }}"
	else
		"${job.handler} · ${job.mode}"

/**
 * What to say about a URL that has been fetched before, or is being fetched now.
 *
 * **Advisory in every case except the live one**, which is the single refusal
 * the server would issue anyway — see `FetchUrlUiState.canSubmit`. A finished
 * fetch is only reported: the panel cannot know that a second copy is not what
 * was wanted, which is the same rule the names' own note follows.
 *
 * A failed or cancelled attempt is phrased as neither a warning nor an
 * apology. It is a reason to try again, and it also explains why nothing turned
 * up in uploads — which is otherwise the most confusing outcome of the three.
 */
private fun duplicateNote(job: FetchJobDto?): String? {
	if (job == null) return null
	val state = FetchState.of(job.state)
	val when_ = relativeTime(job.finished) ?: "recently"
	return when {
		state.isLive -> "You are already fetching this URL — it is listed below."
		state == FetchState.DONE ->
			"You fetched this URL $when_, and it produced ${job.files} file(s). " +
				"Fetching it again makes a second copy."
		state == FetchState.ERROR -> "The last attempt at this URL failed $when_."
		state == FetchState.CANCELLED -> "You cancelled this URL $when_."
		else -> null
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

/**
 * As on the server editor: a full-window form is the one place left where the
 * content is not already bounded by a pane.
 */
private val FORM_MAX_WIDTH = 640.dp
