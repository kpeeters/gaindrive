package org.gaindrive.android.ui.browse

import androidx.compose.animation.animateContentSize
import androidx.compose.foundation.clickable
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.aspectRatio
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.lazy.items
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.automirrored.filled.ArrowBack
import androidx.compose.material3.AssistChip
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
import androidx.compose.ui.platform.LocalUriHandler
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.dp
import androidx.hilt.navigation.compose.hiltViewModel
import androidx.lifecycle.compose.collectAsStateWithLifecycle
import org.gaindrive.android.data.model.AlbumNotes
import org.gaindrive.android.ui.components.CoverHero
import org.gaindrive.android.ui.components.LoadStateBox
import org.gaindrive.android.ui.components.TrackRow

@OptIn(ExperimentalMaterial3Api::class)
@Composable
fun AlbumDetailScreen(
	onBack: () -> Unit,
	viewModel: AlbumDetailViewModel = hiltViewModel(),
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
					item(key = "notes") { NotesBlock(notes) }
				}

				items(ui.detail.songs, key = { it.ref.encode() }) { song ->
					TrackRow(
						song = song,
						// Playback arrives in Phase 3; until then a tap on a
						// track has nothing meaningful to do.
						onClick = {},
					)
				}
			}
		}
	}
}

/**
 * Album notes with the same clamp-and-expand behaviour as the web client:
 * a few lines by default, tap "more" for the rest.
 */
@Composable
private fun NotesBlock(notes: AlbumNotes) {
	var expanded by remember { mutableStateOf(false) }
	val uriHandler = LocalUriHandler.current

	Column(
		modifier = Modifier.padding(16.dp),
		verticalArrangement = Arrangement.spacedBy(8.dp),
	) {
		notes.notes?.let { text ->
			Text(
				text = text,
				style = MaterialTheme.typography.bodyMedium,
				color = MaterialTheme.colorScheme.onSurfaceVariant,
				maxLines = if (expanded) Int.MAX_VALUE else COLLAPSED_LINES,
				overflow = TextOverflow.Ellipsis,
				modifier = Modifier
					.animateContentSize()
					.clickable { expanded = !expanded },
			)
			Text(
				text = if (expanded) "less" else "more",
				style = MaterialTheme.typography.labelMedium,
				color = MaterialTheme.colorScheme.primary,
				modifier = Modifier.clickable { expanded = !expanded },
			)
		}

		Row(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
			notes.wikiUrl?.let { url ->
				AssistChip(
					onClick = { uriHandler.openUri(url) },
					label = { Text("Wikipedia") },
				)
			}
			notes.allMusicUrl?.let { url ->
				AssistChip(
					onClick = { uriHandler.openUri(url) },
					label = { Text("AllMusic") },
				)
			}
		}
	}
}

private const val COLLAPSED_LINES = 4
