package org.gaindrive.android.ui.browse

import androidx.compose.foundation.ExperimentalFoundationApi
import androidx.compose.foundation.background
import androidx.compose.foundation.clickable
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.lazy.items
import androidx.compose.foundation.lazy.rememberLazyListState
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.filled.AddLink
import androidx.compose.material3.ExperimentalMaterial3Api
import androidx.compose.material3.FilterChip
import androidx.compose.material3.HorizontalDivider
import androidx.compose.material3.Icon
import androidx.compose.material3.ListItem
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Scaffold
import androidx.compose.material3.Text
import androidx.compose.material3.TopAppBar
import androidx.compose.runtime.Composable
import androidx.compose.runtime.getValue
import androidx.compose.runtime.remember
import androidx.compose.runtime.rememberCoroutineScope
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.unit.dp
import androidx.hilt.navigation.compose.hiltViewModel
import androidx.lifecycle.compose.collectAsStateWithLifecycle
import kotlinx.coroutines.launch
import org.gaindrive.android.data.model.ArtistIndex
import org.gaindrive.android.data.model.ItemRef
import org.gaindrive.android.data.model.LibraryMode
import org.gaindrive.android.ui.LocalAvailability
import org.gaindrive.android.ui.components.AlphabetRail
import org.gaindrive.android.ui.components.ArtistRow
import org.gaindrive.android.ui.components.ChipRow
import org.gaindrive.android.ui.components.EmptyMessage
import org.gaindrive.android.ui.components.PartialFailureNote
import org.gaindrive.android.ui.components.RefreshableLoadBox
import org.gaindrive.android.ui.components.LibrarySelector

@OptIn(ExperimentalMaterial3Api::class, ExperimentalFoundationApi::class)
@Composable
fun ArtistsScreen(
	/**
	 * The third argument is the slice the artist was opened from. The mode
	 * itself rather than a bool per slice: two of them are now interesting
	 * below — Uploads decides what may be deleted, Categories decides whether
	 * there is a performer to draw a portrait and a biography for — and a pair
	 * of unlabelled booleans at a call site says neither.
	 */
	onOpenArtist: (List<ItemRef>, String, LibraryMode) -> Unit,
	onFetchUrl: () -> Unit,
	viewModel: ArtistsViewModel = hiltViewModel(),
) {
	val state by viewModel.state.collectAsStateWithLifecycle()
	val isRefreshing by viewModel.isRefreshing.collectAsStateWithLifecycle()
	val servers by viewModel.servers.collectAsStateWithLifecycle()
	val browseScope by viewModel.browseScope.collectAsStateWithLifecycle()
	val badgeNames by viewModel.badgeNames.collectAsStateWithLifecycle()
	val fetches by viewModel.fetches.collectAsStateWithLifecycle()
	val offline by viewModel.offline.collectAsStateWithLifecycle()
	val failures by viewModel.failures.collectAsStateWithLifecycle()

	val modes by viewModel.modes.collectAsStateWithLifecycle()
	val mode by viewModel.mode.collectAsStateWithLifecycle()

	Scaffold(
		topBar = {
			TopAppBar(
				title = {
					// Title and chips share the bar rather than taking a row
					// of their own: there is room here, and a second row costs
					// vertical space on every screen for a control that is
					// used occasionally.
					Row(verticalAlignment = Alignment.CenterVertically) {
						Text("Library")
						if (modes.size > 1) {
							Spacer(Modifier.width(12.dp))
							// ChipRow rather than a plain Row so a narrow
							// screen scrolls the chips instead of clipping
							// them, and so the selected one is scrolled into
							// view on first layout.
							ChipRow(
								selectedIndex = modes.indexOf(mode),
								chipCount = modes.size,
							) {
								modes.forEach { m ->
									FilterChip(
										selected = m == mode,
										onClick = { viewModel.selectMode(m) },
										label = { Text(m.label) },
									)
								}
							}
						}
					}
				},
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
			// Above the list, not over it: what did load is still worth using.
			PartialFailureNote(
				failures = failures,
				onRetry = viewModel::refresh,
				onDismiss = viewModel::dismissFailures,
			)

			// Outside the list rather than an item in it, so it is there while
			// the uploads are still loading, when the load failed, and when
			// there is nothing in them yet — which is exactly when someone wants
			// to put something there. The Uploads chip is only offered to an
			// account that may upload, so reaching this means the rights exist.
			if (mode == LibraryMode.UPLOADS) {
				// The row says what is happening when something is, because this
				// is the screen someone comes back to in order to find out. The
				// shell's strip says the same thing from everywhere else; the two
				// read the same monitor, so they cannot disagree.
				val live = fetches.live
				ListItem(
					headlineContent = { Text("Fetch from a URL") },
					supportingContent = {
						Text(
							when {
								live.isEmpty() ->
									"The server downloads it into your uploads"
								live.size == 1 ->
									fetches.moving?.let { "Fetching — ${it.job.percent}%" }
										?: "1 fetch queued"
								else -> "${live.size} fetches in progress"
							}
						)
					},
					leadingContent = {
						Icon(Icons.Default.AddLink, contentDescription = null)
					},
					modifier = Modifier.clickable(onClick = onFetchUrl),
				)
				HorizontalDivider()
			}

			RefreshableLoadBox(
				state = state,
				isRefreshing = isRefreshing,
				onRefresh = viewModel::refresh,
				onRetry = viewModel::load,
			) { indexes ->
				if (indexes.isEmpty()) {
					// Offline the list is trimmed to what has stored audio, so
					// empty means "nothing downloaded", not "empty library" —
					// and saying the latter would send the user hunting for a
					// problem with their server.
					EmptyMessage(
						when {
							!LocalAvailability.current.online ->
								"Nothing is stored on this device yet. Play or " +
									"download something while online first."
							// Empty uploads is the state everybody starts in,
							// and the row above is the way out of it. Saying
							// "no artists" here would read as a fault.
							mode == LibraryMode.UPLOADS ->
								"Nothing in your uploads yet."
							else -> "This library has no artists yet."
						}
					)
					return@RefreshableLoadBox
				}

				val listState = rememberLazyListState()
				val scope = rememberCoroutineScope()

				// Flat item index of each bucket's header. Each bucket
				// contributes its header plus its artists, so the offsets have
				// to be summed rather than derived from the bucket's position.
				val headerPositions = remember(indexes) { headerPositionsOf(indexes) }

				// Only when the labels really are letters. An admin's Uploads
				// listing is grouped by owner instead, so they are usernames —
				// a rail of those is a strip of words down the edge of the
				// screen, and it is for scrubbing a long alphabetical list
				// rather than for jumping between four people.
				val showRail = indexes.all { it.label.length == 1 }

				Box(modifier = Modifier.fillMaxSize()) {
					LazyColumn(
						state = listState,
						modifier = Modifier
							.fillMaxSize()
							// Keeps long artist names clear of the rail rather
							// than letting them slide underneath it. With no
							// rail it would only be a dead strip.
							.padding(end = if (showRail) 24.dp else 0.dp),
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
									onClick = {
										onOpenArtist(artist.refs, artist.name, mode)
									},
									badges = artist.sources.mapNotNull { badgeNames[it] },
								)
							}
						}
					}

					if (showRail) {
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
}

private fun headerPositionsOf(indexes: List<ArtistIndex>): List<Int> {
	var cursor = 0
	return indexes.map { bucket ->
		val headerAt = cursor
		cursor += 1 + bucket.artists.size
		headerAt
	}
}
