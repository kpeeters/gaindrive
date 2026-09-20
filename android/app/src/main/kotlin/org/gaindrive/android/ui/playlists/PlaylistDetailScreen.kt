package org.gaindrive.android.ui.playlists

import androidx.compose.foundation.layout.PaddingValues
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.lazy.itemsIndexed
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.filled.Close
import androidx.compose.material3.ExperimentalMaterial3Api
import androidx.compose.material3.Icon
import androidx.compose.material3.IconButton
import androidx.compose.material3.Scaffold
import androidx.compose.material3.SnackbarHost
import androidx.compose.material3.SnackbarHostState
import androidx.compose.material3.Text
import androidx.compose.material3.TopAppBar
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.Modifier
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.dp
import androidx.hilt.lifecycle.viewmodel.compose.hiltViewModel
import androidx.lifecycle.compose.collectAsStateWithLifecycle
import org.gaindrive.android.data.cache.PinKind
import org.gaindrive.android.data.model.Song
import org.gaindrive.android.ui.components.EmptyMessage
import org.gaindrive.android.ui.components.PaneBackIcon
import org.gaindrive.android.ui.components.PinAction
import org.gaindrive.android.ui.components.RefreshableLoadBox
import org.gaindrive.android.ui.components.SongRow
import org.gaindrive.android.ui.player.PlayerViewModel
import org.gaindrive.android.ui.player.TrackActionsSheet

@OptIn(ExperimentalMaterial3Api::class)
@Composable
fun PlaylistDetailScreen(
	onBack: (() -> Unit)?,
	viewModel: PlaylistDetailViewModel = hiltViewModel(),
	player: PlayerViewModel = hiltViewModel(),
) {
	val state by viewModel.state.collectAsStateWithLifecycle()
	val isRefreshing by viewModel.isRefreshing.collectAsStateWithLifecycle()
	val removing by viewModel.removing.collectAsStateWithLifecycle()
	val error by viewModel.error.collectAsStateWithLifecycle()
	val playerState by player.state.collectAsStateWithLifecycle()

	val snackbar = remember { SnackbarHostState() }
	var actionsFor by remember { mutableStateOf<Song?>(null) }

	LaunchedEffect(error) {
		error?.let {
			snackbar.showSnackbar(it)
			viewModel.clearError()
		}
	}

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
					Text(viewModel.playlistName, maxLines = 1, overflow = TextOverflow.Ellipsis)
				},
				navigationIcon = { PaneBackIcon(onBack) },
				actions = { PinAction(ref = viewModel.playlistRef, kind = PinKind.PLAYLIST) },
			)
		},
		snackbarHost = { SnackbarHost(snackbar) },
	) { insets ->
		RefreshableLoadBox(
			state = state,
			isRefreshing = isRefreshing,
			onRefresh = viewModel::refresh,
			onRetry = viewModel::load,
			modifier = Modifier.padding(insets),
		) { songs ->
			if (songs.isEmpty()) {
				EmptyMessage("This playlist is empty.")
				return@RefreshableLoadBox
			}

			LazyColumn(
				modifier = Modifier.fillMaxSize(),
				// Same tail gap as an album's tracks; see AlbumDetailScreen.
				contentPadding = PaddingValues(bottom = 16.dp),
			) {
				itemsIndexed(
					items = songs,
					// Position, not id: a playlist may hold the same track
					// twice, and duplicate keys crash a LazyColumn.
					key = { index, row -> "$index-${row.song.ref.encode()}" },
				) { index, row ->
					SongRow(
						song = row.song,
						coverUrl = row.coverUrl,
						// Queues the whole playlist and starts here, the same
						// meaning tapping a track in an album has.
						onClick = { player.play(songs.map { it.song }, index) },
						onLongClick = { actionsFor = row.song },
						playback = playerState.trackStateOf(row.song.ref),
						trailing = {
							IconButton(
								onClick = { viewModel.remove(index) },
								// One removal at a time: the call takes a
								// position, and a second one issued now would
								// carry an index the server has already shifted.
								enabled = !removing,
							) {
								Icon(
									Icons.Default.Close,
									contentDescription = "Remove from playlist",
								)
							}
						},
					)
				}
			}
		}
	}
}
