package org.gaindrive.android.ui.browse

import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.PaddingValues
import androidx.compose.foundation.layout.aspectRatio
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.lazy.itemsIndexed
import androidx.compose.foundation.text.KeyboardOptions
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.automirrored.filled.ArrowBack
import androidx.compose.material.icons.filled.ArrowDropDown
import androidx.compose.material.icons.filled.MoreVert
import androidx.compose.material3.AlertDialog
import androidx.compose.material3.DropdownMenu
import androidx.compose.material3.DropdownMenuItem
import androidx.compose.material3.ExperimentalMaterial3Api
import androidx.compose.material3.Icon
import androidx.compose.material3.IconButton
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.OutlinedButton
import androidx.compose.material3.OutlinedTextField
import androidx.compose.material3.RadioButton
import androidx.compose.material3.Scaffold
import androidx.compose.material3.Text
import androidx.compose.material3.TextButton
import androidx.compose.material3.TopAppBar
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.Modifier
import androidx.compose.ui.text.input.ImeAction
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.dp
import androidx.compose.ui.window.PopupProperties
import androidx.hilt.navigation.compose.hiltViewModel
import androidx.lifecycle.compose.collectAsStateWithLifecycle
import org.gaindrive.android.data.cache.PinKind
import org.gaindrive.android.data.model.MusicRoot
import org.gaindrive.android.data.model.Song
import org.gaindrive.android.ui.components.CoverHero
import org.gaindrive.android.ui.components.ExternalLink
import org.gaindrive.android.ui.components.PinAction
import org.gaindrive.android.ui.components.RefreshableLoadBox
import org.gaindrive.android.ui.components.NotesSection
import org.gaindrive.android.ui.components.SectionHeading
import org.gaindrive.android.ui.components.TrackRow
import org.gaindrive.android.ui.player.PlayerViewModel
import org.gaindrive.android.ui.player.TrackActionsSheet

@OptIn(ExperimentalMaterial3Api::class)
@Composable
fun AlbumDetailScreen(
	onBack: () -> Unit,
	/**
	 * Where to go once an album has been moved into the shared library. Not
	 * [onBack]: the album's own artist folder under uploads may have gone with
	 * it, so one level up is a list that may no longer exist.
	 */
	onPromoted: () -> Unit = onBack,
	viewModel: AlbumDetailViewModel = hiltViewModel(),
	player: PlayerViewModel = hiltViewModel(),
) {
	val state by viewModel.state.collectAsStateWithLifecycle()
	val extras by viewModel.extras.collectAsStateWithLifecycle()
	val isRefreshing by viewModel.isRefreshing.collectAsStateWithLifecycle()
	val playerState by player.state.collectAsStateWithLifecycle()
	var actionsFor by remember { mutableStateOf<Song?>(null) }

	val canPromote by viewModel.canPromote.collectAsStateWithLifecycle()
	val promoting by viewModel.promoting.collectAsStateWithLifecycle()
	val promoted by viewModel.promoted.collectAsStateWithLifecycle()
	val promoteError by viewModel.promoteError.collectAsStateWithLifecycle()
	val roots by viewModel.roots.collectAsStateWithLifecycle()
	val destRoot by viewModel.destRoot.collectAsStateWithLifecycle()
	val folder by viewModel.folder.collectAsStateWithLifecycle()
	val folderSuggestions by viewModel.folderSuggestions.collectAsStateWithLifecycle()
	val deleting by viewModel.deleting.collectAsStateWithLifecycle()
	var confirmPromote by remember { mutableStateOf(false) }
	var confirmDelete by remember { mutableStateOf(false) }

	// The album this screen is showing has either moved to a new id or stopped
	// existing, so there is nothing here left to draw either way. The listing it
	// lands on has already been reloaded by the bumped library revision.
	LaunchedEffect(promoted) {
		if (promoted) onPromoted()
	}

	actionsFor?.let { song ->
		TrackActionsSheet(
			song = song,
			onDismiss = { actionsFor = null },
			onPlayNext = { player.playNext(song) },
			onAddToQueue = { player.addToQueue(song) },
		)
	}

	if (confirmPromote) {
		PromoteDialog(
			albumTitle = viewModel.albumTitle,
			roots = roots,
			chosen = destRoot,
			folder = folder,
			suggestions = folderSuggestions,
			busy = promoting,
			onRoot = viewModel::selectRoot,
			onFolder = viewModel::onFolder,
			onDismiss = { confirmPromote = false },
			onConfirm = {
				confirmPromote = false
				viewModel.promote()
			},
		)
	}

	if (confirmDelete) {
		AlertDialog(
			onDismissRequest = { confirmDelete = false },
			title = { Text("Delete from uploads?") },
			// Says where the files go, because that is the whole difference
			// between this and every other action on the screen. There is no
			// trash behind it and nothing in the app can put them back.
			text = {
				Text(
					"“${viewModel.albumTitle}” and its files are removed from " +
						"the server. This cannot be undone."
				)
			},
			confirmButton = {
				TextButton(
					enabled = !deleting,
					onClick = {
						confirmDelete = false
						viewModel.deleteUpload()
					},
				) {
					Text("Delete", color = MaterialTheme.colorScheme.error)
				}
			},
			dismissButton = {
				TextButton(onClick = { confirmDelete = false }) { Text("Cancel") }
			},
		)
	}

	// One dialog for both failures — it says whatever the server said, and
	// "Could not move it" / "Could not delete it" is already in the message.
	promoteError?.let { message ->
		AlertDialog(
			onDismissRequest = viewModel::clearPromoteError,
			title = { Text("Nothing changed") },
			text = { Text(message) },
			confirmButton = {
				TextButton(onClick = viewModel::clearPromoteError) { Text("OK") }
			},
		)
	}

	Scaffold(
		topBar = {
			TopAppBar(
				title = {
					Text(viewModel.albumTitle, maxLines = 1, overflow = TextOverflow.Ellipsis)
				},
				navigationIcon = {
					IconButton(onClick = onBack) {
						Icon(Icons.AutoMirrored.Filled.ArrowBack, contentDescription = "Back")
					}
				},
				actions = {
					PinAction(ref = viewModel.albumRef, kind = PinKind.ALBUM)
					// An overflow rather than icons of their own: both are rare
					// one-way actions and neither belongs a tap away from Pin.
					// Delete is offered to whoever owns the upload, Move only to
					// an admin, so the menu itself appears for either.
					if (canPromote || viewModel.canDelete) {
						var menuOpen by remember { mutableStateOf(false) }
						IconButton(onClick = { menuOpen = true }) {
							Icon(Icons.Default.MoreVert, contentDescription = "More")
						}
						DropdownMenu(
							expanded = menuOpen,
							onDismissRequest = { menuOpen = false },
						) {
							if (canPromote) {
								DropdownMenuItem(
									text = { Text("Move to library") },
									enabled = !promoting && !deleting,
									onClick = {
										menuOpen = false
										confirmPromote = true
									},
								)
							}
							if (viewModel.canDelete) {
								DropdownMenuItem(
									text = {
										Text(
											"Delete from uploads",
											color = MaterialTheme.colorScheme.error,
										)
									},
									enabled = !promoting && !deleting,
									onClick = {
										menuOpen = false
										confirmDelete = true
									},
								)
							}
						}
					}
				},
			)
		},
	) { insets ->
		RefreshableLoadBox(
			state = state,
			isRefreshing = isRefreshing,
			onRefresh = viewModel::refresh,
			onRetry = viewModel::load,
			modifier = Modifier.padding(insets),
		) { detail ->
			LazyColumn(
				modifier = Modifier.fillMaxSize(),
				// contentPadding rather than a spacer item: it scrolls with the
				// content and needs no key. The last track otherwise ends flush
				// against the mini-player, which reads as a cut-off list.
				contentPadding = PaddingValues(bottom = 16.dp),
			) {
				// The placeholder already occupies the full square, so the
				// artwork arriving later does not move anything below it.
				item(key = "hero") {
					CoverHero(
						url = extras.heroUrl,
						contentDescription = detail.album.title,
						modifier = Modifier
							.fillMaxWidth()
							.aspectRatio(1f)
							.padding(16.dp),
					)
				}

				item(key = "heading") {
					Column(modifier = Modifier.padding(horizontal = 16.dp)) {
						Text(
							text = detail.album.title,
							style = MaterialTheme.typography.headlineSmall,
						)
						Text(
							text = listOfNotNull(
								detail.album.artistName.takeIf { it.isNotBlank() },
								detail.album.year?.toString(),
								detail.album.genre,
							).joinToString(" · "),
							style = MaterialTheme.typography.bodyMedium,
							color = MaterialTheme.colorScheme.onSurfaceVariant,
						)

						// Inside the heading rather than an item of its own:
						// the notes arrive after the tracks are on screen, and
						// a new item above the list would shift the rows out
						// from under the user's finger.
						extras.notes?.let { notes ->
							NotesSection(
								text = notes.notes,
								links = buildList {
									notes.wikiUrl?.let { add(ExternalLink("Wikipedia", it)) }
									notes.allMusicUrl?.let { add(ExternalLink("AllMusic", it)) }
								},
								modifier = Modifier.padding(vertical = 12.dp),
							)
						}
					}
				}

				// A "Disc 1" banner on an album that has only one disc says
				// nothing, so the headings appear only where they separate
				// something. The songs already arrive ordered by disc.
				val multiDisc =
					detail.songs.mapTo(mutableSetOf()) { it.discNumber ?: 1 }.size > 1

				// An album ripped with no tags, or with every track tagged 1,
				// carries no usable numbering — number the rows by position
				// instead of leaving the column blank. The server derives a
				// number from a numbered filename, so this is the remainder:
				// files named without one. Matches web/app.js, album-wide index
				// included, so the two clients read the same.
				val useSeq = detail.songs.all { (it.track ?: 0) <= 1 }

				itemsIndexed(
					items = detail.songs,
					key = { _, song -> song.ref.encode() },
				) { index, song ->
					// The heading rides along with the first track of its disc
					// rather than being an item of its own, so the list keys
					// stay one per song.
					Column {
						if (multiDisc) {
							val disc = song.discNumber ?: 1
							val previous = detail.songs.getOrNull(index - 1)
							if (previous == null || (previous.discNumber ?: 1) != disc) {
								// season carries the same number as discNumber
								// and is set only when the group really is a
								// season, so a show reads "Series 2" while a
								// two-disc film still reads "Disc 2". Per group
								// rather than per album: a show's unnumbered
								// Specials folder is a disc.
								val kind = if (song.season != null) "Series" else "Disc"
								SectionHeading("$kind $disc")
							}
						}
						TrackRow(
							song = song,
							// Queues the whole album and starts here, which is
							// what tapping a track in an album listing should
							// mean.
							onClick = { player.play(detail.songs, index) },
							onLongClick = { actionsFor = song },
							playback = playerState.trackStateOf(song.ref),
							number = if (useSeq) index + 1 else song.track,
						)
					}
				}
			}
		}
	}
}


/**
 * Where an upload goes when it is moved into the shared library.
 *
 * Two controls, not a folder browser, and that is not a simplification: both
 * layouts are `L1/L2/[L3]/files`, the album being moved *is* L2, and L3 is only
 * ever a disc or season directory inside it. So a root and one level under it is
 * the whole of the destination — there is no third level to walk to.
 *
 * The root picker is hidden when there is only one, the same rule the library
 * selector and the fetch panel's server picker use.
 */
@OptIn(ExperimentalMaterial3Api::class)
@Composable
private fun PromoteDialog(
	albumTitle: String,
	roots: List<MusicRoot>,
	chosen: MusicRoot?,
	folder: String,
	suggestions: List<String>,
	busy: Boolean,
	onRoot: (MusicRoot) -> Unit,
	onFolder: (String) -> Unit,
	onDismiss: () -> Unit,
	onConfirm: () -> Unit,
) {
	val isCategories = chosen?.contentType == "categories"

	AlertDialog(
		onDismissRequest = onDismiss,
		title = { Text("Move to the library") },
		text = {
			Column(verticalArrangement = Arrangement.spacedBy(12.dp)) {
				// Says what happens on the server's disk, because that is what
				// makes this different from every other action on this screen:
				// it is not undoable from the app, and everyone else browsing
				// the server sees the result.
				Text(
					"“$albumTitle” leaves your uploads and joins the shared " +
						"library, where everyone with an account can see it. " +
						"The files move on the server; this cannot be undone " +
						"from here."
				)

				if (roots.size > 1) {
					var open by remember { mutableStateOf(false) }
					Box {
						OutlinedButton(enabled = !busy, onClick = { open = true }) {
							Text(chosen?.name ?: "Choose a library")
							Icon(Icons.Default.ArrowDropDown, contentDescription = null)
						}
						DropdownMenu(expanded = open, onDismissRequest = { open = false }) {
							roots.forEach { root ->
								DropdownMenuItem(
									text = {
										// The kind alongside the name: which one
										// is the film library is not guessable
										// from an operator's chosen name.
										Text(
											root.contentType
												?.let { "${root.name} ($it)" }
												?: root.name
										)
									},
									leadingIcon = {
										RadioButton(
											selected = root.id == chosen?.id,
											onClick = null,
										)
									},
									onClick = {
										open = false
										onRoot(root)
									},
								)
							}
						}
					}
				}

				// Free text with suggestions rather than a picker: typing a name
				// that is not in the list is how a new artist or category is
				// made, and the server creates the directory, so there is no
				// separate operation for it.
				var menuOpen by remember { mutableStateOf(false) }
				val matches = remember(folder, suggestions) {
					if (folder.isBlank()) suggestions.take(SUGGESTION_LIMIT)
					else suggestions
						.filter { it.contains(folder, ignoreCase = true) && it != folder }
						.take(SUGGESTION_LIMIT)
				}
				Box {
					OutlinedTextField(
						value = folder,
						onValueChange = {
							onFolder(it)
							menuOpen = true
						},
						label = { Text(if (isCategories) "Category" else "Artist") },
						supportingText = {
							Text(
								if (isCategories) "A category that does not exist yet is created."
								else "An artist that does not exist yet is created."
							)
						},
						singleLine = true,
						enabled = !busy,
						keyboardOptions = KeyboardOptions(imeAction = ImeAction.Done),
						modifier = Modifier.fillMaxWidth(),
					)
					DropdownMenu(
						expanded = menuOpen && matches.isNotEmpty(),
						onDismissRequest = { menuOpen = false },
						// Never takes the keyboard focus: this sits under a text
						// field that is still being typed into.
						properties = PopupProperties(focusable = false),
					) {
						matches.forEach { name ->
							DropdownMenuItem(
								text = { Text(name) },
								onClick = {
									menuOpen = false
									onFolder(name)
								},
							)
						}
					}
				}
			}
		},
		confirmButton = {
			TextButton(
				// Both halves are required by the server, so the button says so
				// rather than letting a refusal arrive afterwards. It also
				// catches a categories root left with the field empty, which
				// used to file a documentary under the channel that published
				// it.
				enabled = !busy && chosen != null && folder.isNotBlank(),
				onClick = onConfirm,
			) { Text("Move") }
		},
		dismissButton = { TextButton(onClick = onDismiss) { Text("Cancel") } },
	)
}

/** Enough to be useful in a dialog, few enough not to cover the field. */
private const val SUGGESTION_LIMIT = 6
