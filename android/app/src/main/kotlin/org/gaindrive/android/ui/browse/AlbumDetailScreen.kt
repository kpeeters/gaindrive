package org.gaindrive.android.ui.browse

import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.aspectRatio
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.lazy.itemsIndexed
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.automirrored.filled.ArrowBack
import androidx.compose.material3.ExperimentalMaterial3Api
import androidx.compose.material3.Icon
import androidx.compose.material3.IconButton
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Scaffold
import androidx.compose.material3.Text
import androidx.compose.material3.TopAppBar
import androidx.compose.runtime.Composable
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.Modifier
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.dp
import androidx.hilt.navigation.compose.hiltViewModel
import androidx.lifecycle.compose.collectAsStateWithLifecycle
import org.gaindrive.android.data.cache.PinKind
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
	viewModel: AlbumDetailViewModel = hiltViewModel(),
	player: PlayerViewModel = hiltViewModel(),
) {
	val state by viewModel.state.collectAsStateWithLifecycle()
	val extras by viewModel.extras.collectAsStateWithLifecycle()
	val isRefreshing by viewModel.isRefreshing.collectAsStateWithLifecycle()
	val playerState by player.state.collectAsStateWithLifecycle()
	var actionsFor by remember { mutableStateOf<Song?>(null) }

	actionsFor?.let { song ->
		TrackActionsSheet(
			song = song,
			onDismiss = { actionsFor = null },
			onPlayNext = { player.playNext(song) },
			onAddToQueue = { player.addToQueue(song) },
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
				actions = { PinAction(ref = viewModel.albumRef, kind = PinKind.ALBUM) },
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
			LazyColumn(modifier = Modifier.fillMaxSize()) {
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
								SectionHeading("Disc $disc")
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
						)
					}
				}
			}
		}
	}
}

