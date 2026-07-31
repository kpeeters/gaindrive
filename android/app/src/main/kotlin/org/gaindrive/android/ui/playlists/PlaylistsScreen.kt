package org.gaindrive.android.ui.playlists

import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.lazy.items
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.filled.Delete
import androidx.compose.material.icons.filled.MoreVert
import androidx.compose.material3.AlertDialog
import androidx.compose.material3.DropdownMenu
import androidx.compose.material3.DropdownMenuItem
import androidx.compose.material3.ExperimentalMaterial3Api
import androidx.compose.material3.Icon
import androidx.compose.material3.IconButton
import androidx.compose.material3.Scaffold
import androidx.compose.material3.SnackbarHost
import androidx.compose.material3.SnackbarHostState
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
import androidx.hilt.navigation.compose.hiltViewModel
import androidx.lifecycle.compose.collectAsStateWithLifecycle
import org.gaindrive.android.data.model.ItemRef
import org.gaindrive.android.data.model.Playlist
import org.gaindrive.android.ui.LocalAvailability
import org.gaindrive.android.ui.components.EmptyMessage
import org.gaindrive.android.ui.components.PartialFailureNote
import org.gaindrive.android.ui.components.PlaylistRow
import org.gaindrive.android.ui.components.RefreshableLoadBox
import org.gaindrive.android.ui.components.SectionHeading
import org.gaindrive.android.ui.components.LibrarySelector

@OptIn(ExperimentalMaterial3Api::class)
@Composable
fun PlaylistsScreen(
	onOpenPlaylist: (ItemRef, String) -> Unit,
	viewModel: PlaylistsViewModel = hiltViewModel(),
) {
	val state by viewModel.state.collectAsStateWithLifecycle()
	val isRefreshing by viewModel.isRefreshing.collectAsStateWithLifecycle()
	val servers by viewModel.servers.collectAsStateWithLifecycle()
	val browseScope by viewModel.browseScope.collectAsStateWithLifecycle()
	val badgeNames by viewModel.badgeNames.collectAsStateWithLifecycle()
	val offline by viewModel.offline.collectAsStateWithLifecycle()
	val failures by viewModel.failures.collectAsStateWithLifecycle()
	val error by viewModel.error.collectAsStateWithLifecycle()

	val snackbar = remember { SnackbarHostState() }
	var confirming by remember { mutableStateOf<Playlist?>(null) }

	// A failed edit is transient and does not survive a rotation; a modal per
	// failed request would be far too heavy here.
	LaunchedEffect(error) {
		error?.let {
			snackbar.showSnackbar(it)
			viewModel.clearError()
		}
	}

	confirming?.let { playlist ->
		AlertDialog(
			onDismissRequest = { confirming = null },
			title = { Text("Delete playlist?") },
			text = { Text("“${playlist.name}” will be removed from the server.") },
			confirmButton = {
				TextButton(onClick = {
					viewModel.delete(playlist)
					confirming = null
				}) { Text("Delete") }
			},
			dismissButton = {
				TextButton(onClick = { confirming = null }) { Text("Cancel") }
			},
		)
	}

	Scaffold(
		topBar = {
			TopAppBar(
				title = { Text("Playlists") },
				actions = {
					LibrarySelector(
						servers = servers,
						scope = browseScope,
						offline = offline,
						onSelectAll = viewModel::selectAllServers,
						onSelect = viewModel::selectServer,
						onSetOffline = viewModel::setOffline,
					)
				},
			)
		},
		snackbarHost = { SnackbarHost(snackbar) },
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
			) { sections ->
				if (sections.all { it.items.isEmpty() }) {
					EmptyMessage(
						if (LocalAvailability.current.online) {
							"No playlists yet. Long-press a track and choose " +
								"“Add to playlist” to make one."
						} else {
							"No playlist has any of its tracks stored on this " +
								"device."
						}
					)
					return@RefreshableLoadBox
				}

				LazyColumn(modifier = Modifier.fillMaxSize()) {
					sections.forEach { section ->
						// Named only when there is more than one server in
						// play; otherwise the heading is a label on the
						// obvious.
						badgeNames[section.server.id]?.let { name ->
							item(key = "hdr-${section.server.id.value}") {
								SectionHeading(name)
							}
						}
						items(section.items, key = { it.ref.encode() }) { playlist ->
							PlaylistRow(
								playlist = playlist,
								onClick = { onOpenPlaylist(playlist.ref, playlist.name) },
								trailing = {
									PlaylistMenu(onDelete = { confirming = playlist })
								},
							)
						}
					}
				}
			}
		}
	}
}

@Composable
private fun PlaylistMenu(onDelete: () -> Unit) {
	var open by remember { mutableStateOf(false) }

	IconButton(onClick = { open = true }) {
		Icon(Icons.Default.MoreVert, contentDescription = "Playlist actions")
		DropdownMenu(expanded = open, onDismissRequest = { open = false }) {
			DropdownMenuItem(
				text = { Text("Delete") },
				leadingIcon = { Icon(Icons.Default.Delete, contentDescription = null) },
				onClick = {
					open = false
					onDelete()
				},
			)
		}
	}
}
