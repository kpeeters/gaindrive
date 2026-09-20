package org.gaindrive.android.ui.browse

import androidx.compose.foundation.ExperimentalFoundationApi
import androidx.compose.foundation.background
import androidx.compose.foundation.clickable
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.lazy.items
import androidx.compose.foundation.lazy.rememberLazyListState
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.automirrored.filled.ArrowBack
import androidx.compose.material.icons.filled.AddLink
import androidx.compose.material.icons.filled.Upload
import androidx.compose.material3.ExperimentalMaterial3Api
import androidx.compose.material3.HorizontalDivider
import androidx.compose.material3.Icon
import androidx.compose.material3.IconButton
import androidx.compose.material3.ListItem
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
import androidx.hilt.lifecycle.viewmodel.compose.hiltViewModel
import androidx.lifecycle.compose.collectAsStateWithLifecycle
import kotlinx.coroutines.launch
import org.gaindrive.android.data.model.ArtistIndex
import org.gaindrive.android.data.model.ItemRef
import org.gaindrive.android.data.model.LibrarySection
import org.gaindrive.android.ui.LocalAvailability
import org.gaindrive.android.ui.components.AlphabetRail
import org.gaindrive.android.ui.components.ArtistRow
import org.gaindrive.android.ui.components.EmptyMessage
import org.gaindrive.android.ui.components.PartialFailureNote
import org.gaindrive.android.ui.components.RefreshableLoadBox
import org.gaindrive.android.ui.components.LibrarySelector

@OptIn(ExperimentalMaterial3Api::class, ExperimentalFoundationApi::class)
@Composable
fun ArtistsScreen(
	/**
	 * The third argument is the section the artist was opened from. The
	 * section itself rather than a bool per case: two of them are interesting
	 * below — Uploads decides what may be deleted, Categories decides whether
	 * there is a performer to draw a portrait and a biography for — and a pair
	 * of unlabelled booleans at a call site says neither.
	 */
	onOpenArtist: (List<ItemRef>, String, LibrarySection) -> Unit,
	onFetchUrl: () -> Unit,
	/** Set only on the main library, where the upload icon leads away. */
	onOpenUploads: (() -> Unit)? = null,
	/** Set only on the pushed uploads listing, which has somewhere to go back to. */
	onBack: (() -> Unit)? = null,
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
	val canUpload by viewModel.canUpload.collectAsStateWithLifecycle()

	val uploads = viewModel.uploads

	Scaffold(
		topBar = {
			TopAppBar(
				title = { Text(if (uploads) "Uploads" else "Library") },
				navigationIcon = {
					if (onBack != null) {
						IconButton(onClick = onBack) {
							Icon(Icons.AutoMirrored.Filled.ArrowBack, contentDescription = "Back")
						}
					}
				},
				actions = {
					// Only where it leads somewhere else: on the uploads
					// listing itself the icon would be a door into the room
					// you are standing in.
					if (onOpenUploads != null && canUpload) {
						IconButton(onClick = onOpenUploads) {
							Icon(Icons.Default.Upload, contentDescription = "Uploads")
						}
					}
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
			// to put something there. The uploads listing is only reachable by
			// an account that may upload, so reaching this means the rights
			// exist.
			if (uploads) {
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
			) { listing ->
				if (listing.isEmpty) {
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
							uploads -> "Nothing in your uploads yet."
							else -> "This library has no artists yet."
						}
					)
					return@RefreshableLoadBox
				}

				val listState = rememberLazyListState()
				val scope = rememberCoroutineScope()

				val hasCategories = listing.categories.isNotEmpty()

				// Flat item index of each artist bucket's header, offset past
				// the Categories section when there is one — its header plus
				// its rows sit above the first artist bucket.
				val headerPositions = remember(listing) {
					val offset = if (hasCategories) 1 + listing.categories.size else 0
					headerPositionsOf(listing.artists, offset)
				}

				// Only when the labels really are letters, and only over the
				// artist buckets — the Categories header is not a rail stop.
				// An admin's uploads listing is grouped by owner instead, so
				// its labels are usernames — a rail of those is a strip of
				// words down the edge of the screen, and it is for scrubbing a
				// long alphabetical list rather than for jumping between four
				// people.
				val showRail = listing.artists.isNotEmpty() &&
					listing.artists.all { it.label.length == 1 }

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
						// The whole group under one heading: a library holds a
						// handful of sections, not enough to bucket by letter.
						// The key cannot collide with a bucket's "hdr-C" — no
						// bucket label is more than one character when it is a
						// letter at all.
						if (hasCategories) {
							stickyHeader(key = "hdr-Categories") {
								IndexHeader("Categories")
							}
							items(listing.categories, key = { it.ref.encode() }) { artist ->
								ArtistRow(
									artist = artist,
									onClick = {
										onOpenArtist(
											artist.refs,
											artist.name,
											LibrarySection.CATEGORIES,
										)
									},
									badges = artist.sources.mapNotNull { badgeNames[it] },
								)
							}
						}

						listing.artists.forEach { index ->
							stickyHeader(key = "hdr-${index.label}") {
								IndexHeader(index.label)
							}
							items(index.artists, key = { it.ref.encode() }) { artist ->
								ArtistRow(
									artist = artist,
									onClick = {
										onOpenArtist(
											artist.refs,
											artist.name,
											if (uploads) LibrarySection.UPLOADS
											else LibrarySection.ARTISTS,
										)
									},
									badges = artist.sources.mapNotNull { badgeNames[it] },
								)
							}
						}
					}

					if (showRail) {
						AlphabetRail(
							letters = listing.artists.map { it.label },
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

@Composable
private fun IndexHeader(label: String) {
	Text(
		text = label,
		style = MaterialTheme.typography.labelLarge,
		color = MaterialTheme.colorScheme.primary,
		modifier = Modifier
			.fillMaxWidth()
			.background(MaterialTheme.colorScheme.background)
			.padding(horizontal = 16.dp, vertical = 6.dp),
	)
}

private fun headerPositionsOf(indexes: List<ArtistIndex>, offset: Int): List<Int> {
	var cursor = offset
	return indexes.map { bucket ->
		val headerAt = cursor
		cursor += 1 + bucket.artists.size
		headerAt
	}
}
