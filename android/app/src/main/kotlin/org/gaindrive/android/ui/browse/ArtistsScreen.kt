package org.gaindrive.android.ui.browse

import androidx.compose.foundation.ExperimentalFoundationApi
import androidx.compose.foundation.background
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.lazy.items
import androidx.compose.foundation.lazy.rememberLazyListState
import androidx.compose.material3.ExperimentalMaterial3Api
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Scaffold
import androidx.compose.material3.Text
import androidx.compose.material3.TopAppBar
import androidx.compose.runtime.Composable
import androidx.compose.runtime.getValue
import androidx.compose.runtime.remember
import androidx.compose.runtime.rememberCoroutineScope
import androidx.compose.ui.Modifier
import androidx.compose.ui.unit.dp
import androidx.hilt.navigation.compose.hiltViewModel
import androidx.lifecycle.compose.collectAsStateWithLifecycle
import kotlinx.coroutines.launch
import org.gaindrive.android.data.model.ArtistIndex
import org.gaindrive.android.data.model.ItemRef
import org.gaindrive.android.ui.components.AlphabetRail
import org.gaindrive.android.ui.components.ArtistRow
import org.gaindrive.android.ui.components.EmptyMessage
import org.gaindrive.android.ui.components.PartialFailureNote
import org.gaindrive.android.ui.components.RefreshableLoadBox
import org.gaindrive.android.ui.components.ServerSelector

@OptIn(ExperimentalMaterial3Api::class, ExperimentalFoundationApi::class)
@Composable
fun ArtistsScreen(
	onOpenArtist: (List<ItemRef>, String) -> Unit,
	viewModel: ArtistsViewModel = hiltViewModel(),
) {
	val state by viewModel.state.collectAsStateWithLifecycle()
	val isRefreshing by viewModel.isRefreshing.collectAsStateWithLifecycle()
	val servers by viewModel.servers.collectAsStateWithLifecycle()
	val browseScope by viewModel.browseScope.collectAsStateWithLifecycle()
	val badgeNames by viewModel.badgeNames.collectAsStateWithLifecycle()
	val failures by viewModel.failures.collectAsStateWithLifecycle()

	Scaffold(
		topBar = {
			TopAppBar(
				title = { Text("Artists") },
				actions = {
					ServerSelector(
						servers = servers,
						scope = browseScope,
						onSelectAll = viewModel::selectAllServers,
						onSelect = viewModel::selectServer,
					)
				},
			)
		},
	) { insets ->
		Column(modifier = Modifier.padding(insets)) {
			// Above the list, not over it: what did load is still worth using.
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
			) { indexes ->
				if (indexes.isEmpty()) {
					EmptyMessage("This library has no artists yet.")
					return@RefreshableLoadBox
				}

				val listState = rememberLazyListState()
				val scope = rememberCoroutineScope()

				// Flat item index of each bucket's header. Each bucket
				// contributes its header plus its artists, so the offsets have
				// to be summed rather than derived from the bucket's position.
				val headerPositions = remember(indexes) { headerPositionsOf(indexes) }

				Box(modifier = Modifier.fillMaxSize()) {
					LazyColumn(
						state = listState,
						modifier = Modifier
							.fillMaxSize()
							// Keeps long artist names clear of the rail rather
							// than letting them slide underneath it.
							.padding(end = 24.dp),
					) {
						indexes.forEach { index ->
							stickyHeader(key = "hdr-${index.label}") {
								Text(
									text = index.label,
									style = MaterialTheme.typography.labelLarge,
									color = MaterialTheme.colorScheme.primary,
									modifier = Modifier
										.fillMaxWidth()
										.background(MaterialTheme.colorScheme.background)
										.padding(horizontal = 16.dp, vertical = 6.dp),
								)
							}
							items(index.artists, key = { it.ref.encode() }) { artist ->
								ArtistRow(
									artist = artist,
									onClick = { onOpenArtist(artist.refs, artist.name) },
									badges = artist.sources.mapNotNull { badgeNames[it] },
								)
							}
						}
					}

					AlphabetRail(
						letters = indexes.map { it.label },
						onSelect = { position ->
							scope.launch {
								// Jump, not animate: scrubbing the rail issues
								// these in quick succession and animations
								// would queue up and lag behind the finger.
								listState.scrollToItem(headerPositions[position])
							}
						},
						modifier = Modifier.fillMaxSize(),
					)
				}
			}
		}
	}
}

private fun headerPositionsOf(indexes: List<ArtistIndex>): List<Int> {
	var cursor = 0
	return indexes.map { bucket ->
		val headerAt = cursor
		cursor += 1 + bucket.artists.size
		headerAt
	}
}
