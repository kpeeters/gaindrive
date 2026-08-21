package org.gaindrive.android.ui.player

import androidx.compose.foundation.clickable
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.aspectRatio
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.lazy.itemsIndexed
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.filled.Cast
import androidx.compose.material.icons.filled.CastConnected
import androidx.compose.material.icons.filled.Close
import androidx.compose.material.icons.filled.Info
import androidx.compose.material.icons.filled.Movie
import androidx.compose.material.icons.filled.Pause
import androidx.compose.material.icons.filled.PlayArrow
import androidx.compose.material.icons.filled.SkipNext
import androidx.compose.material.icons.filled.SkipPrevious
import androidx.compose.material3.ExperimentalMaterial3Api
import androidx.compose.material3.Icon
import androidx.compose.material3.IconButton
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.ModalBottomSheet
import androidx.compose.material3.Text
import androidx.compose.material3.rememberModalBottomSheetState
import androidx.compose.runtime.Composable
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.text.style.TextAlign
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.dp
import org.gaindrive.android.data.model.ItemRef
import org.gaindrive.android.playback.PlayerState
import org.gaindrive.android.ui.components.CoverHero
import org.gaindrive.android.ui.components.CoverThumb

@OptIn(ExperimentalMaterial3Api::class)
@Composable
fun NowPlayingSheet(
	state: PlayerState,
	casting: Boolean,
	onDismiss: () -> Unit,
	onOpenAlbum: (ItemRef, String) -> Unit,
	onTogglePlay: () -> Unit,
	onNext: () -> Unit,
	onPrevious: () -> Unit,
	onSeek: (Long) -> Unit,
	onJumpTo: (Int) -> Unit,
	onCast: () -> Unit,
	onInfo: () -> Unit,
	onRemoveFromQueue: (Int) -> Unit,
	onWatch: () -> Unit,
) {
	val current = state.current ?: return
	val sheetState = rememberModalBottomSheetState(skipPartiallyExpanded = true)

	// The way out of a single search hit and into the rest of the record. Both
	// the sleeve and the artist · album line under the title lead here; null
	// when the queue entry has no album ref, which leaves both inert.
	val openAlbum: (() -> Unit)? = current.albumRef?.let { ref ->
		{ onOpenAlbum(ref, current.album) }
	}

	ModalBottomSheet(onDismissRequest = onDismiss, sheetState = sheetState) {
		LazyColumn(modifier = Modifier.fillMaxWidth()) {
			item(key = "art") {
				CoverHero(
					url = current.artworkUrl,
					contentDescription =
						if (openAlbum == null) current.album
						else "Open album ${current.album}",
					modifier = Modifier
						.fillMaxWidth()
						.aspectRatio(1f)
						.padding(horizontal = 32.dp),
					onClick = openAlbum,
				)
			}

			item(key = "meta") {
				Column(
					modifier = Modifier
						.fillMaxWidth()
						.padding(horizontal = 24.dp, vertical = 16.dp),
				) {
					Text(
						text = current.title,
						style = MaterialTheme.typography.titleLarge,
						maxLines = 2,
						overflow = TextOverflow.Ellipsis,
						textAlign = TextAlign.Center,
						modifier = Modifier.fillMaxWidth(),
					)
					// Tinted primary when it leads somewhere: the sleeve alone is
					// not a discoverable way through to the album.
					Text(
						text = listOf(current.artist, current.album)
							.filter { it.isNotBlank() }
							.joinToString(" · "),
						style = MaterialTheme.typography.bodyMedium,
						color = if (openAlbum == null) {
							MaterialTheme.colorScheme.onSurfaceVariant
						} else {
							MaterialTheme.colorScheme.primary
						},
						maxLines = 1,
						overflow = TextOverflow.Ellipsis,
						textAlign = TextAlign.Center,
						modifier = Modifier
							.fillMaxWidth()
							.then(
								if (openAlbum == null) {
									Modifier
								} else {
									Modifier.clickable(
										onClickLabel = "Open album",
										onClick = openAlbum,
									)
								}
							)
							// Inside the ripple, so a single line of body text is
							// still a usable target. Applied either way, so the
							// layout does not shift when there is no album to
							// open.
							.padding(vertical = 8.dp),
					)
				}
			}

			item(key = "seek") { SeekBar(state, onSeek) }

			item(key = "controls") {
				// A Box rather than one Row: the transport stays centred on the
				// screen whether or not the cast button is beside it, which a
				// trailing item in a centred Row would quietly break.
				Box(modifier = Modifier.fillMaxWidth()) {
					Row(
						modifier = Modifier.align(Alignment.Center),
						horizontalArrangement = Arrangement.Center,
						verticalAlignment = Alignment.CenterVertically,
					) {
						IconButton(onClick = onPrevious, enabled = state.hasPrevious) {
							Icon(
								Icons.Default.SkipPrevious,
								contentDescription = "Previous",
								modifier = Modifier.size(36.dp),
							)
						}
						IconButton(onClick = onTogglePlay) {
							Icon(
								imageVector = if (state.isPlaying) {
									Icons.Default.Pause
								} else {
									Icons.Default.PlayArrow
								},
								contentDescription = if (state.isPlaying) "Pause" else "Play",
								modifier = Modifier.size(48.dp),
							)
						}
						IconButton(onClick = onNext, enabled = state.hasNext) {
							Icon(
								Icons.Default.SkipNext,
								contentDescription = "Next",
								modifier = Modifier.size(36.dp),
							)
						}
					}
					// The way back to the picture after backing out of the video
					// screen, which leaves the sound playing. Without it the
					// only route back would be to start the film again.
					if (state.isVideo) {
						IconButton(
							onClick = onWatch,
							modifier = Modifier
								.align(Alignment.CenterStart)
								.padding(start = 12.dp),
						) {
							Icon(
								imageVector = Icons.Default.Movie,
								contentDescription = "Watch",
								tint = MaterialTheme.colorScheme.primary,
							)
						}
					}
					// A Row rather than two aligned buttons, so the transport
					// stays centred whatever the trailing pair adds up to —
					// which is the same reason the Box above exists.
					Row(
						modifier = Modifier
							.align(Alignment.CenterEnd)
							.padding(end = 12.dp),
						verticalAlignment = Alignment.CenterVertically,
					) {
						// Always offered, unlike cast: it has no outcome that
						// is only a refusal, and what it answers — how the
						// audio is reaching the speaker, and in what format —
						// is nowhere else in the UI.
						IconButton(onClick = onInfo) {
							Icon(
								Icons.Default.Info,
								contentDescription = "Track info",
								tint = MaterialTheme.colorScheme.onSurfaceVariant,
							)
						}
						// Offered for a video only when the receiver could
						// actually play it: one the server can hand over as a
						// seekable MP4. For the rest a button whose only
						// outcome is a refusal is worse than no button. Still
						// shown while casting, so the way to disconnect stays
						// where it always is.
						if (!state.isVideo || state.nativeSeek || casting) {
							IconButton(onClick = onCast) {
								Icon(
									imageVector = if (casting) {
										Icons.Default.CastConnected
									} else {
										Icons.Default.Cast
									},
									contentDescription = if (casting) "Casting" else "Cast",
									tint = if (casting) {
										MaterialTheme.colorScheme.primary
									} else {
										MaterialTheme.colorScheme.onSurfaceVariant
									},
								)
							}
						}
					}
				}
			}

			if (state.queue.size > 1) {
				item(key = "queue-heading") {
					Text(
						text = "Queue",
						style = MaterialTheme.typography.titleMedium,
						color = MaterialTheme.colorScheme.primary,
						modifier = Modifier.padding(start = 24.dp, top = 24.dp, bottom = 8.dp),
					)
				}
				itemsIndexed(state.queue) { index, entry ->
					Row(
						modifier = Modifier
							.fillMaxWidth()
							.clickable { onJumpTo(index) }
							.padding(horizontal = 24.dp, vertical = 8.dp),
						verticalAlignment = Alignment.CenterVertically,
						horizontalArrangement = Arrangement.spacedBy(12.dp),
					) {
						CoverThumb(entry.artworkUrl, entry.album, size = 32.dp)
						Column(modifier = Modifier.weight(1f)) {
							Text(
								text = entry.title,
								style = MaterialTheme.typography.bodyMedium,
								color = if (index == state.queueIndex) {
									MaterialTheme.colorScheme.primary
								} else {
									MaterialTheme.colorScheme.onSurface
								},
								maxLines = 1,
								overflow = TextOverflow.Ellipsis,
							)
							Text(
								text = entry.artist,
								style = MaterialTheme.typography.bodySmall,
								color = MaterialTheme.colorScheme.onSurfaceVariant,
								maxLines = 1,
								overflow = TextOverflow.Ellipsis,
							)
						}
						// The track playing cannot be removed: dropping it would
						// mean deciding what plays instead, which is what skip is
						// for.
						if (index != state.queueIndex) {
							IconButton(onClick = { onRemoveFromQueue(index) }) {
								Icon(
									Icons.Default.Close,
									contentDescription = "Remove from queue",
								)
							}
						}
					}
				}
			}

			item(key = "close") {
				Row(
					modifier = Modifier.fillMaxWidth().padding(16.dp),
					horizontalArrangement = Arrangement.Center,
				) {
					IconButton(onClick = onDismiss) {
						Icon(Icons.Default.Close, contentDescription = "Close")
					}
				}
			}
		}
	}
}

