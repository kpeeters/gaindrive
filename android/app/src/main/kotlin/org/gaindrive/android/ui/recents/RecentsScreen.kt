package org.gaindrive.android.ui.recents

import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.lazy.itemsIndexed
import androidx.compose.material3.ExperimentalMaterial3Api
import androidx.compose.material3.Scaffold
import androidx.compose.material3.Text
import androidx.compose.material3.TopAppBar
import androidx.compose.runtime.Composable
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.Modifier
import androidx.hilt.lifecycle.viewmodel.compose.hiltViewModel
import androidx.lifecycle.compose.collectAsStateWithLifecycle
import org.gaindrive.android.data.model.ItemRef
import org.gaindrive.android.data.model.Song
import org.gaindrive.android.ui.claimsFocus
import org.gaindrive.android.ui.components.EmptyMessage
import org.gaindrive.android.ui.components.PartialFailureNote
import org.gaindrive.android.ui.components.RefreshableLoadBox
import org.gaindrive.android.ui.components.SectionHeading
import org.gaindrive.android.ui.components.LibrarySelector
import org.gaindrive.android.ui.components.SongRow
import org.gaindrive.android.ui.components.relativeTime
import org.gaindrive.android.ui.player.PlayerViewModel
import org.gaindrive.android.ui.player.TrackActionsSheet

@OptIn(ExperimentalMaterial3Api::class)
@Composable
fun RecentsScreen(
	onOpenAlbum: (ItemRef, String) -> Unit,
	viewModel: RecentsViewModel = hiltViewModel(),
	player: PlayerViewModel = hiltViewModel(),
) {
	val state by viewModel.state.collectAsStateWithLifecycle()
	val isRefreshing by viewModel.isRefreshing.collectAsStateWithLifecycle()
	val servers by viewModel.servers.collectAsStateWithLifecycle()
	val browseScope by viewModel.browseScope.collectAsStateWithLifecycle()
	val badgeNames by viewModel.badgeNames.collectAsStateWithLifecycle()
	val offline by viewModel.offline.collectAsStateWithLifecycle()
	val failures by viewModel.failures.collectAsStateWithLifecycle()
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
				title = { Text("Recents") },
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
					EmptyMessage("Nothing played yet.")
					return@RefreshableLoadBox
				}

				LazyColumn(modifier = Modifier.fillMaxSize().claimsFocus()) {
					sections.forEach { section ->
						badgeNames[section.server.id]?.let { name ->
							item(key = "hdr-${section.server.id.value}") {
								SectionHeading(name)
							}
						}
						itemsIndexed(
							items = section.items,
							// A track played twice appears twice, so the id
							// alone is not unique here.
							key = { index, row ->
								"${section.server.id.value}-$index-${row.song.ref.encode()}"
							},
						) { _, row ->
							SongRow(
								song = row.song,
								coverUrl = row.coverUrl,
								// Both, deliberately: playback starts here, and
								// the album opens so the rest of the record is
								// one tap away. This is what the web client
								// does from a listing.
								onClick = {
									viewModel.playInAlbumContext(row.song)
									row.song.albumRef?.let {
										onOpenAlbum(it, row.song.albumTitle)
									}
								},
								onLongClick = { actionsFor = row.song },
								playback = playerState.trackStateOf(row.song.ref),
								trailingText = relativeTime(row.song.lastPlayedAt),
							)
						}
					}
				}
			}
		}
	}
}
