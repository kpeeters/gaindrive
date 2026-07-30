package org.gaindrive.android.ui.browse

import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.lazy.items
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.automirrored.filled.ArrowBack
import androidx.compose.material3.ExperimentalMaterial3Api
import androidx.compose.material3.HorizontalDivider
import androidx.compose.material3.Icon
import androidx.compose.material3.IconButton
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Scaffold
import androidx.compose.material3.Text
import androidx.compose.material3.TopAppBar
import androidx.compose.runtime.Composable
import androidx.compose.runtime.getValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.dp
import androidx.hilt.navigation.compose.hiltViewModel
import androidx.lifecycle.compose.collectAsStateWithLifecycle
import org.gaindrive.android.data.model.ItemRef
import org.gaindrive.android.ui.components.AlbumRow
import org.gaindrive.android.ui.components.ArtistAvatar
import org.gaindrive.android.ui.components.EmptyMessage
import org.gaindrive.android.ui.components.ExternalLink
import org.gaindrive.android.ui.components.RefreshableLoadBox
import org.gaindrive.android.ui.components.NotesSection

@OptIn(ExperimentalMaterial3Api::class)
@Composable
fun AlbumsScreen(
	onBack: () -> Unit,
	onOpenAlbum: (ItemRef, String) -> Unit,
	viewModel: AlbumsViewModel = hiltViewModel(),
) {
	val state by viewModel.state.collectAsStateWithLifecycle()
	val isRefreshing by viewModel.isRefreshing.collectAsStateWithLifecycle()
	val header by viewModel.header.collectAsStateWithLifecycle()

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
				navigationIcon = {
					IconButton(onClick = onBack) {
						Icon(Icons.AutoMirrored.Filled.ArrowBack, contentDescription = "Back")
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
		) { albums ->
			LazyColumn(modifier = Modifier.fillMaxSize()) {
				item(key = "header") {
					ArtistHeader(
						name = viewModel.artistName,
						header = header,
						albumCount = albums.size,
					)
				}

				if (albums.isEmpty()) {
					item(key = "empty") {
						EmptyMessage("No albums for this artist.")
					}
				}

				items(albums, key = { it.album.ref.encode() }) { row ->
					AlbumRow(row.album, row.coverUrl) {
						onOpenAlbum(row.album.ref, row.album.title)
					}
				}
			}
		}
	}
}

/**
 * Portrait, name and biography. Appears immediately with the placeholder
 * avatar and fills in as the pieces arrive, rather than making the album list
 * wait for a lookup that may never succeed.
 */
@Composable
private fun ArtistHeader(
	name: String,
	header: ArtistHeaderUi,
	albumCount: Int,
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
			ArtistAvatar(url = header.portraitUrl, contentDescription = name)
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
