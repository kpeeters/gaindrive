package org.gaindrive.android.ui.search

import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
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
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.OutlinedTextField
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
import org.gaindrive.android.data.model.ItemRef
import org.gaindrive.android.data.model.Song
import org.gaindrive.android.playback.TrackState
import org.gaindrive.android.ui.components.AlbumRow
import org.gaindrive.android.ui.components.ArtistRow
import org.gaindrive.android.ui.components.EmptyMessage
import org.gaindrive.android.ui.components.SongRow
import org.gaindrive.android.ui.player.PlayerViewModel
import org.gaindrive.android.ui.player.TrackActionsSheet

@Composable
fun SearchScreen(
	onOpenArtist: (ItemRef, String) -> Unit,
	onOpenAlbum: (ItemRef, String) -> Unit,
	viewModel: SearchViewModel = hiltViewModel(),
	player: PlayerViewModel = hiltViewModel(),
) {
	val query by viewModel.query.collectAsStateWithLifecycle()
	val filters by viewModel.filters.collectAsStateWithLifecycle()
	val phase by viewModel.phase.collectAsStateWithLifecycle()
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

	Column(modifier = Modifier.fillMaxSize()) {
		SearchHeader(
			query = query,
			filters = filters,
			onQueryChange = viewModel::onQueryChange,
			onClear = viewModel::clearQuery,
			onToggleArtists = viewModel::toggleArtists,
			onToggleAlbums = viewModel::toggleAlbums,
			onToggleSongs = viewModel::toggleSongs,
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

			is SearchPhase.Ready -> {
				if (current.results.isEmpty) {
					EmptyMessage("Nothing matched “$query”.")
				} else {
					Results(
						results = current.results,
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

@Composable
private fun SearchHeader(
	query: String,
	filters: SearchFilters,
	onQueryChange: (String) -> Unit,
	onClear: () -> Unit,
	onToggleArtists: () -> Unit,
	onToggleAlbums: () -> Unit,
	onToggleSongs: () -> Unit,
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

			Row(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
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
			}
		}
	}
}

@Composable
private fun Results(
	results: SearchResults,
	onOpenArtist: (ItemRef, String) -> Unit,
	onOpenAlbum: (ItemRef, String) -> Unit,
	onPlaySong: (Song) -> Unit,
	onSongActions: (Song) -> Unit,
	playbackOf: (ItemRef) -> TrackState,
) {
	LazyColumn(modifier = Modifier.fillMaxSize()) {
		if (results.artists.isNotEmpty()) {
			item(key = "h-artists") { SectionHeading("Artists") }
			items(results.artists, key = { "a-${it.ref.encode()}" }) { artist ->
				ArtistRow(artist) { onOpenArtist(artist.ref, artist.name) }
			}
		}

		if (results.albums.isNotEmpty()) {
			item(key = "h-albums") { SectionHeading("Albums") }
			items(results.albums, key = { "al-${it.album.ref.encode()}" }) { row ->
				AlbumRow(row.album, row.coverUrl) {
					onOpenAlbum(row.album.ref, row.album.title)
				}
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
				)
			}
		}
	}
}

@Composable
private fun SectionHeading(text: String) {
	Text(
		text = text,
		style = MaterialTheme.typography.titleSmall,
		color = MaterialTheme.colorScheme.primary,
		modifier = Modifier.padding(start = 16.dp, top = 16.dp, bottom = 4.dp),
	)
}
