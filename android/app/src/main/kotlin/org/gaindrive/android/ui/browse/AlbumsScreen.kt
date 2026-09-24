package org.gaindrive.android.ui.browse

import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.lazy.itemsIndexed
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.automirrored.filled.Sort
import androidx.compose.material3.DropdownMenu
import androidx.compose.material3.DropdownMenuItem
import androidx.compose.material3.ExperimentalMaterial3Api
import androidx.compose.material3.HorizontalDivider
import androidx.compose.material3.Icon
import androidx.compose.material3.IconButton
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.RadioButton
import androidx.compose.material3.Scaffold
import androidx.compose.material3.Text
import androidx.compose.material3.TopAppBar
import androidx.compose.runtime.Composable
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.dp
import androidx.hilt.lifecycle.viewmodel.compose.hiltViewModel
import androidx.lifecycle.compose.collectAsStateWithLifecycle
import org.gaindrive.android.data.model.AlbumSort
import org.gaindrive.android.data.model.ItemRef
import org.gaindrive.android.ui.LocalAvailability
import org.gaindrive.android.ui.claimsFocus
import org.gaindrive.android.ui.components.AlbumRow
import org.gaindrive.android.ui.components.ArtistAvatar
import org.gaindrive.android.ui.components.EmptyMessage
import org.gaindrive.android.ui.components.ExternalLink
import org.gaindrive.android.ui.components.PaneBackIcon
import org.gaindrive.android.ui.components.PartialFailureNote
import org.gaindrive.android.ui.components.RefreshableLoadBox
import org.gaindrive.android.ui.components.NotesSection

@OptIn(ExperimentalMaterial3Api::class)
@Composable
fun AlbumsScreen(
	/** Null while the artist list is on screen beside this pane. */
	onBack: (() -> Unit)?,
	/** The third argument carries "this album is a personal upload" downwards. */
	onOpenAlbum: (ItemRef, String, Boolean) -> Unit,
	viewModel: AlbumsViewModel = hiltViewModel(),
) {
	val state by viewModel.state.collectAsStateWithLifecycle()
	val isRefreshing by viewModel.isRefreshing.collectAsStateWithLifecycle()
	val header by viewModel.header.collectAsStateWithLifecycle()
	val failures by viewModel.failures.collectAsStateWithLifecycle()
	val sort by viewModel.sort.collectAsStateWithLifecycle()

	Scaffold(
		topBar = {
			TopAppBar(
				title = {
					Text(
						text = viewModel.artistName,
						maxLines = 1,
						overflow = TextOverflow.Ellipsis,
					)
				},
				navigationIcon = { PaneBackIcon(onBack) },
				actions = { SortAction(current = sort, onSelect = viewModel::setSort) },
			)
		},
	) { insets ->
		Column(modifier = Modifier.padding(insets)) {
			PartialFailureNote(
				failures = failures,
				onRetry = viewModel::refresh,
				onDismiss = viewModel::dismissFailures,
			)

			RefreshableLoadBox(
				state = state,
				isRefreshing = isRefreshing,
				onRefresh = viewModel::refresh,
				onRetry = viewModel::load,
			) { albums ->
				LazyColumn(modifier = Modifier.fillMaxSize()) {
					item(key = "header") {
						ArtistHeader(
							name = viewModel.artistName,
							header = header,
							albumCount = albums.size,
							showPortrait = !viewModel.isCategory,
						)
					}

					if (albums.isEmpty()) {
						item(key = "empty") {
							EmptyMessage(
								if (LocalAvailability.current.online) {
									"No albums for this artist."
								} else {
									"None of this artist's albums are stored " +
										"on this device."
								}
							)
						}
					}

					// The first album rather than the list: the header above it
					// can hold a focusable biography, and landing there would
					// cost a press before the albums are reached.
					itemsIndexed(albums, key = { _, row -> row.album.ref.encode() }) { index, row ->
						AlbumRow(
							album = row.album,
							coverUrl = row.coverUrl,
							onClick = {
							onOpenAlbum(row.album.ref, row.album.title, viewModel.fromUploads)
						},
							badges = row.badges,
							modifier = if (index == 0) Modifier.claimsFocus() else Modifier,
						)
					}
				}
			}
		}
	}
}

/**
 * Which order the albums are listed in.
 *
 * A menu rather than a button that cycles: there are two orders now and the
 * radio says which one is showing, where a single icon would only say that
 * *something* can be sorted. The Albums pane is reached from Search as well as
 * from the Library, so this appears in both.
 */
@Composable
private fun SortAction(current: AlbumSort, onSelect: (AlbumSort) -> Unit) {
	var open by remember { mutableStateOf(false) }

	Box {
		IconButton(onClick = { open = true }) {
			Icon(
				imageVector = Icons.AutoMirrored.Filled.Sort,
				contentDescription = "Sort albums, currently by ${current.label.lowercase()}",
			)
		}
		DropdownMenu(expanded = open, onDismissRequest = { open = false }) {
			AlbumSort.entries.forEach { order ->
				DropdownMenuItem(
					leadingIcon = { RadioButton(selected = order == current, onClick = null) },
					text = { Text(order.label) },
					onClick = {
						open = false
						onSelect(order)
					},
				)
			}
		}
	}
}

/**
 * Portrait, name and biography. Appears immediately with the placeholder
 * avatar and fills in as the pieces arrive, rather than making the album list
 * wait for a lookup that may never succeed.
 *
 * [showPortrait] is false for a section of a categories root, which has no
 * performer behind it: the avatar would stay a placeholder for ever, and
 * reserving 96dp for it says a picture is coming. The biography needs no such
 * flag - it is drawn only once one has arrived.
 */
@Composable
private fun ArtistHeader(
	name: String,
	header: ArtistHeaderUi,
	albumCount: Int,
	showPortrait: Boolean,
) {
	Column(
		modifier = Modifier
			.fillMaxWidth()
			.padding(16.dp),
		verticalArrangement = Arrangement.spacedBy(12.dp),
	) {
		Row(
			verticalAlignment = Alignment.CenterVertically,
			horizontalArrangement = Arrangement.spacedBy(16.dp),
		) {
			if (showPortrait) {
				ArtistAvatar(url = header.portraitUrl, contentDescription = name)
			}
			Column {
				Text(text = name, style = MaterialTheme.typography.headlineSmall)
				Text(
					text = if (albumCount == 1) "1 album" else "$albumCount albums",
					style = MaterialTheme.typography.bodyMedium,
					color = MaterialTheme.colorScheme.onSurfaceVariant,
				)
			}
		}

		header.info?.let { info ->
			NotesSection(
				text = info.biography,
				links = buildList {
					info.wikiUrl?.let { add(ExternalLink("Wikipedia", it)) }
					info.allMusicUrl?.let { add(ExternalLink("AllMusic", it)) }
					info.lastFmUrl?.let { add(ExternalLink("Last.fm", it)) }
					info.discogsUrl?.let { add(ExternalLink("Discogs", it)) }
				},
			)
		}
	}

	HorizontalDivider(color = MaterialTheme.colorScheme.outlineVariant)
}
