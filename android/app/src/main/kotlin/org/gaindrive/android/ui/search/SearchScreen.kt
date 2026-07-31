package org.gaindrive.android.ui.search

import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.lazy.items
import androidx.compose.foundation.text.KeyboardActions
import androidx.compose.foundation.text.KeyboardOptions
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.filled.Close
import androidx.compose.material.icons.filled.Search
import androidx.compose.material3.CircularProgressIndicator
import androidx.compose.material3.FilterChip
import androidx.compose.material3.Icon
import androidx.compose.material3.IconButton
import androidx.compose.material3.LinearProgressIndicator
import androidx.compose.material3.ExperimentalMaterial3Api
import androidx.compose.material3.OutlinedTextField
import androidx.compose.material3.pulltorefresh.PullToRefreshBox
import androidx.compose.material3.Surface
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.focus.FocusRequester
import androidx.compose.ui.focus.focusRequester
import androidx.compose.ui.platform.LocalSoftwareKeyboardController
import androidx.compose.ui.text.input.ImeAction
import androidx.compose.ui.unit.dp
import androidx.hilt.navigation.compose.hiltViewModel
import androidx.lifecycle.compose.collectAsStateWithLifecycle
import org.gaindrive.android.data.model.BrowseScope
import org.gaindrive.android.data.model.ItemRef
import org.gaindrive.android.data.model.ServerConfig
import org.gaindrive.android.data.model.ServerId
import org.gaindrive.android.data.model.Song
import org.gaindrive.android.playback.TrackState
import org.gaindrive.android.ui.components.AlbumRow
import org.gaindrive.android.ui.components.ArtistRow
import org.gaindrive.android.ui.components.EmptyMessage
import org.gaindrive.android.ui.components.PartialFailureNote
import org.gaindrive.android.ui.components.SectionHeading
import org.gaindrive.android.ui.components.LibrarySelector
import org.gaindrive.android.ui.components.SongRow
import org.gaindrive.android.ui.player.PlayerViewModel
import org.gaindrive.android.ui.player.TrackActionsSheet

@OptIn(ExperimentalMaterial3Api::class)
@Composable
fun SearchScreen(
	onOpenArtist: (List<ItemRef>, String) -> Unit,
	onOpenAlbum: (ItemRef, String) -> Unit,
	viewModel: SearchViewModel = hiltViewModel(),
	player: PlayerViewModel = hiltViewModel(),
) {
	val query by viewModel.query.collectAsStateWithLifecycle()
	val filters by viewModel.filters.collectAsStateWithLifecycle()
	val phase by viewModel.phase.collectAsStateWithLifecycle()
	val playerState by player.state.collectAsStateWithLifecycle()
	val isRefreshing by viewModel.isRefreshing.collectAsStateWithLifecycle()
	val servers by viewModel.servers.collectAsStateWithLifecycle()
	val browseScope by viewModel.browseScope.collectAsStateWithLifecycle()
	val badgeNames by viewModel.badgeNames.collectAsStateWithLifecycle()
	val offline by viewModel.offline.collectAsStateWithLifecycle()

	var actionsFor by remember { mutableStateOf<Song?>(null) }

	// Held here rather than in the view model: dismissing a note must not cost
	// a second fan-out across every server.
	var notesDismissed by remember { mutableStateOf(false) }
	LaunchedEffect(query) { notesDismissed = false }

	actionsFor?.let { song ->
		TrackActionsSheet(
			song = song,
			onDismiss = { actionsFor = null },
			onPlayNext = { player.playNext(song) },
			onAddToQueue = { player.addToQueue(song) },
		)
	}

	Column(modifier = Modifier.fillMaxSize()) {
		SearchHeader(
			query = query,
			filters = filters,
			servers = servers,
			scope = browseScope,
			offline = offline,
			onQueryChange = viewModel::onQueryChange,
			onClear = viewModel::clearQuery,
			onToggleArtists = viewModel::toggleArtists,
			onToggleAlbums = viewModel::toggleAlbums,
			onToggleSongs = viewModel::toggleSongs,
			onSelectAll = viewModel::selectAllServers,
			onSelectServer = viewModel::selectServer,
			onSetOffline = viewModel::setOffline,
		)

		when (val current = phase) {
			is SearchPhase.Idle -> EmptyMessage(
				if (filters.noneSelected) {
					"Choose at least one category to search."
				} else {
					"Search for artists, albums and tracks."
				}
			)

			is SearchPhase.Searching -> Box(
				modifier = Modifier.fillMaxSize(),
				contentAlignment = Alignment.Center,
			) {
				CircularProgressIndicator()
			}

			is SearchPhase.Failed -> EmptyMessage(current.message)

			is SearchPhase.Ready -> Column {
				if (!notesDismissed) {
					PartialFailureNote(
						failures = current.failures,
						onRetry = viewModel::refresh,
						onDismiss = { notesDismissed = true },
					)
				}
				// Quiet, and above the results rather than in place of them:
				// what has arrived is already usable.
				if (current.outstanding) {
					LinearProgressIndicator(modifier = Modifier.fillMaxWidth())
				}

				PullToRefreshBox(
					isRefreshing = isRefreshing,
					onRefresh = viewModel::refresh,
				) {
					if (current.results.isEmpty) {
						// Not "nothing matched" while servers are still
						// answering — that would be a claim we cannot make yet.
						if (current.outstanding) {
							EmptyMessage("Searching…")
						} else {
							EmptyMessage("Nothing matched “$query”.")
						}
					} else {
						Results(
							results = current.results,
							badgeNames = badgeNames,
							onOpenArtist = onOpenArtist,
							onOpenAlbum = onOpenAlbum,
							onPlaySong = { song -> player.play(listOf(song), 0) },
							onSongActions = { song -> actionsFor = song },
							playbackOf = playerState::trackStateOf,
						)
					}
				}
			}
		}
	}
}

@Composable
private fun SearchHeader(
	query: String,
	filters: SearchFilters,
	servers: List<ServerConfig>,
	scope: BrowseScope,
	offline: Boolean,
	onQueryChange: (String) -> Unit,
	onClear: () -> Unit,
	onToggleArtists: () -> Unit,
	onToggleAlbums: () -> Unit,
	onToggleSongs: () -> Unit,
	onSelectAll: () -> Unit,
	onSelectServer: (ServerId) -> Unit,
	onSetOffline: (Boolean) -> Unit,
) {
	val focusRequester = remember { FocusRequester() }
	val keyboard = LocalSoftwareKeyboardController.current

	// Opening a tab called Search and having to tap the field first is a wasted
	// step; the field is the whole point of the screen.
	LaunchedEffect(Unit) {
		focusRequester.requestFocus()
	}

	Surface(tonalElevation = 2.dp) {
		Column(
			modifier = Modifier.padding(horizontal = 16.dp, vertical = 8.dp),
			verticalArrangement = Arrangement.spacedBy(8.dp),
		) {
			OutlinedTextField(
				value = query,
				onValueChange = onQueryChange,
				placeholder = { Text("Search") },
				leadingIcon = { Icon(Icons.Default.Search, contentDescription = null) },
				trailingIcon = {
					if (query.isNotEmpty()) {
						IconButton(onClick = onClear) {
							Icon(Icons.Default.Close, contentDescription = "Clear")
						}
					}
				},
				singleLine = true,
				keyboardOptions = KeyboardOptions(imeAction = ImeAction.Search),
				// Results are already live as you type, so the Search key has
				// nothing to submit — its useful job is getting the keyboard
				// out of the way of them.
				keyboardActions = KeyboardActions(onSearch = { keyboard?.hide() }),
				modifier = Modifier
					.fillMaxWidth()
					.focusRequester(focusRequester),
			)

			Row(
				horizontalArrangement = Arrangement.spacedBy(8.dp),
				verticalAlignment = Alignment.CenterVertically,
			) {
				FilterChip(
					selected = filters.artists,
					onClick = onToggleArtists,
					label = { Text("Artists") },
				)
				FilterChip(
					selected = filters.albums,
					onClick = onToggleAlbums,
					label = { Text("Albums") },
				)
				FilterChip(
					selected = filters.songs,
					onClick = onToggleSongs,
					label = { Text("Tracks") },
				)
				Spacer(modifier = Modifier.weight(1f))
				// Search has no app bar of its own, so the scope lives with the
				// filters — it is one more thing narrowing what comes back.
				LibrarySelector(
					servers = servers,
					scope = scope,
					offline = offline,
					onSelectAll = onSelectAll,
					onSelect = onSelectServer,
					onSetOffline = onSetOffline,
				)
			}
		}
	}
}

@Composable
private fun Results(
	results: SearchResults,
	badgeNames: Map<ServerId, String>,
	onOpenArtist: (List<ItemRef>, String) -> Unit,
	onOpenAlbum: (ItemRef, String) -> Unit,
	onPlaySong: (Song) -> Unit,
	onSongActions: (Song) -> Unit,
	playbackOf: (ItemRef) -> TrackState,
) {
	LazyColumn(modifier = Modifier.fillMaxSize()) {
		if (results.artists.isNotEmpty()) {
			item(key = "h-artists") { SectionHeading("Artists") }
			items(results.artists, key = { "a-${it.ref.encode()}" }) { artist ->
				ArtistRow(
					artist = artist,
					onClick = { onOpenArtist(artist.refs, artist.name) },
					badges = artist.sources.mapNotNull { badgeNames[it] },
				)
			}
		}

		if (results.albums.isNotEmpty()) {
			item(key = "h-albums") { SectionHeading("Albums") }
			items(results.albums, key = { "al-${it.album.ref.encode()}" }) { row ->
				AlbumRow(
					album = row.album,
					coverUrl = row.coverUrl,
					onClick = { onOpenAlbum(row.album.ref, row.album.title) },
					badges = row.album.sources.mapNotNull { badgeNames[it] },
				)
			}
		}

		if (results.songs.isNotEmpty()) {
			item(key = "h-songs") { SectionHeading("Tracks") }
			items(results.songs, key = { "s-${it.song.ref.encode()}" }) { row ->
				SongRow(
					song = row.song,
					coverUrl = row.coverUrl,
					onClick = { onPlaySong(row.song) },
					onLongClick = { onSongActions(row.song) },
					playback = playbackOf(row.song.ref),
					badge = badgeNames[row.song.ref.server],
				)
			}
		}
	}
}
