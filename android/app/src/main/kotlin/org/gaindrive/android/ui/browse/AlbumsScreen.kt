package org.gaindrive.android.ui.browse

import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.lazy.items
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.automirrored.filled.ArrowBack
import androidx.compose.material3.ExperimentalMaterial3Api
import androidx.compose.material3.Icon
import androidx.compose.material3.IconButton
import androidx.compose.material3.Scaffold
import androidx.compose.material3.Text
import androidx.compose.material3.TopAppBar
import androidx.compose.runtime.Composable
import androidx.compose.runtime.getValue
import androidx.compose.ui.Modifier
import androidx.compose.ui.text.style.TextOverflow
import androidx.hilt.navigation.compose.hiltViewModel
import androidx.lifecycle.compose.collectAsStateWithLifecycle
import org.gaindrive.android.data.model.ItemRef
import org.gaindrive.android.ui.components.AlbumRow
import org.gaindrive.android.ui.components.EmptyMessage
import org.gaindrive.android.ui.components.LoadStateBox

@OptIn(ExperimentalMaterial3Api::class)
@Composable
fun AlbumsScreen(
	onBack: () -> Unit,
	onOpenAlbum: (ItemRef, String) -> Unit,
	viewModel: AlbumsViewModel = hiltViewModel(),
) {
	val state by viewModel.state.collectAsStateWithLifecycle()

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
		LoadStateBox(
			state = state,
			onRetry = viewModel::load,
			modifier = Modifier.padding(insets),
		) { albums ->
			if (albums.isEmpty()) {
				EmptyMessage("No albums for this artist.")
				return@LoadStateBox
			}
			LazyColumn(modifier = Modifier.fillMaxSize()) {
				items(albums, key = { it.album.ref.encode() }) { row ->
					AlbumRow(row.album, row.coverUrl) {
						onOpenAlbum(row.album.ref, row.album.title)
					}
				}
			}
		}
	}
}
