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
import androidx.compose.ui.Modifier
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.dp
import androidx.hilt.navigation.compose.hiltViewModel
import androidx.lifecycle.compose.collectAsStateWithLifecycle
import org.gaindrive.android.ui.components.CoverHero
import org.gaindrive.android.ui.components.ExternalLink
import org.gaindrive.android.ui.components.LoadStateBox
import org.gaindrive.android.ui.components.NotesSection
import org.gaindrive.android.ui.components.TrackRow
import org.gaindrive.android.ui.player.PlayerViewModel

@OptIn(ExperimentalMaterial3Api::class)
@Composable
fun AlbumDetailScreen(
	onBack: () -> Unit,
	viewModel: AlbumDetailViewModel = hiltViewModel(),
	player: PlayerViewModel = hiltViewModel(),
) {
	val state by viewModel.state.collectAsStateWithLifecycle()

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
			)
		},
	) { insets ->
		LoadStateBox(
			state = state,
			onRetry = viewModel::load,
			modifier = Modifier.padding(insets),
		) { ui ->
			LazyColumn(modifier = Modifier.fillMaxSize()) {
				item(key = "hero") {
					CoverHero(
						url = ui.heroUrl,
						contentDescription = ui.detail.album.title,
						modifier = Modifier
							.fillMaxWidth()
							.aspectRatio(1f)
							.padding(16.dp),
					)
				}

				item(key = "heading") {
					Column(modifier = Modifier.padding(horizontal = 16.dp)) {
						Text(
							text = ui.detail.album.title,
							style = MaterialTheme.typography.headlineSmall,
						)
						Text(
							text = listOfNotNull(
								ui.detail.album.artistName.takeIf { it.isNotBlank() },
								ui.detail.album.year?.toString(),
								ui.detail.album.genre,
							).joinToString(" · "),
							style = MaterialTheme.typography.bodyMedium,
							color = MaterialTheme.colorScheme.onSurfaceVariant,
						)
					}
				}

				ui.detail.notes?.let { notes ->
					item(key = "notes") {
						NotesSection(
							text = notes.notes,
							links = buildList {
								notes.wikiUrl?.let { add(ExternalLink("Wikipedia", it)) }
								notes.allMusicUrl?.let { add(ExternalLink("AllMusic", it)) }
							},
							modifier = Modifier.padding(16.dp),
						)
					}
				}

				itemsIndexed(
					items = ui.detail.songs,
					key = { _, song -> song.ref.encode() },
				) { index, song ->
					TrackRow(
						song = song,
						// Queues the whole album and starts here, which is what
						// tapping a track in an album listing should mean.
						onClick = { player.play(ui.detail.songs, index) },
					)
				}
			}
		}
	}
}

