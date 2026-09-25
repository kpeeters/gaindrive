package org.gaindrive.android.ui.player

import android.util.Log
import androidx.compose.foundation.clickable
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.aspectRatio
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.lazy.itemsIndexed
import androidx.compose.foundation.lazy.rememberLazyListState
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.filled.Cast
import androidx.compose.material.icons.filled.CastConnected
import androidx.compose.material.icons.filled.Close
import androidx.compose.material.icons.filled.DragHandle
import androidx.compose.material.icons.filled.GraphicEq
import androidx.compose.material.icons.filled.Info
import androidx.compose.material.icons.filled.KeyboardArrowDown
import androidx.compose.material.icons.filled.KeyboardArrowUp
import androidx.compose.material.icons.filled.Movie
import androidx.compose.material.icons.filled.Pause
import androidx.compose.material.icons.filled.PlayArrow
import androidx.compose.material.icons.filled.SkipNext
import androidx.compose.material.icons.filled.SkipPrevious
import androidx.compose.material.icons.filled.Tune
import androidx.compose.material3.ExperimentalMaterial3Api
import androidx.compose.material3.Icon
import androidx.compose.material3.IconButton
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.ModalBottomSheet
import androidx.compose.material3.Surface
import androidx.compose.material3.Text
import androidx.compose.material3.rememberModalBottomSheetState
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.rememberUpdatedState
import androidx.compose.runtime.setValue
import androidx.compose.runtime.withFrameNanos
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.focus.FocusRequester
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.text.style.TextAlign
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.dp
import org.gaindrive.android.data.model.ItemRef
import org.gaindrive.android.data.model.StarKind
import org.gaindrive.android.playback.PlayerState
import org.gaindrive.android.playback.NowPlaying
import org.gaindrive.android.ui.LocalIsTv
import org.gaindrive.android.ui.components.CoverHero
import org.gaindrive.android.ui.components.CoverThumb
import org.gaindrive.android.ui.components.StarButton
import sh.calvin.reorderable.ReorderableItem
import sh.calvin.reorderable.rememberReorderableLazyListState

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
	onCast: (() -> Unit)?,
	onSpeakerControls: () -> Unit,
	onEqualizer: () -> Unit,
	onInfo: () -> Unit,
	onRemoveFromQueue: (Int) -> Unit,
	onMoveInQueue: (from: Int, to: Int) -> Unit,
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

	// On TV the sheet opens with the d-pad on the seek bar, where center
	// pauses and left/right skip (TV-PC). The square cover is taller than a
	// 540dp screen, so the bar is not composed until the list scrolls past
	// the cover; the title stays in view above it.
	val isTv = LocalIsTv.current
	val listState = rememberLazyListState()
	val seekFocus = remember { FocusRequester() }
	if (isTv) {
		LaunchedEffect(Unit) {
			listState.scrollToItem(META_INDEX)
			repeat(FOCUS_FRAMES) {
				withFrameNanos {}
				if (seekFocus.requestFocus()) return@LaunchedEffect
			}
			Log.w(TAG, "seek bar never became focusable")
		}
	}

	ModalBottomSheet(onDismissRequest = onDismiss, sheetState = sheetState) {
		// The queue as drawn, which during a drag is the local order the rows
		// have been dragged into. It follows the player the rest of the time;
		// publish() rebuilds the queue on every player event, and following it
		// mid-drag would snap the lifted row back. One move is sent on drop.
		val published = remember(state.queue) { queueRows(state.queue) }
		var rows by remember { mutableStateOf(published) }
		var dragging by remember { mutableStateOf<String?>(null) }
		LaunchedEffect(published) {
			if (dragging == null) rows = published
		}
		// The drag handle's pointer input is started once and keeps the
		// lambdas it was first given, so the drop reads the queue through this
		// rather than capturing a copy that is stale by the second move.
		val latestPublished by rememberUpdatedState(published)
		// By key, not index, so the highlight stays on the playing track while
		// it is being dragged past others.
		val currentKey = published.getOrNull(state.queueIndex)?.key
		// Only queue rows are ReorderableItems, so the cover, controls and
		// headings around them are never offered as drop targets.
		val reorderState = rememberReorderableLazyListState(listState) { from, to ->
			rows = rows.toMutableList().apply {
				val f = indexOfFirst { it.key == from.key }
				val t = indexOfFirst { it.key == to.key }
				if (f >= 0 && t >= 0) add(t, removeAt(f))
			}
		}

		LazyColumn(state = listState, modifier = Modifier.fillMaxWidth()) {
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
					// The star sits beside the title rather than in the
					// transport row, whose sides are already full on a phone.
					// A spacer of the same width on the left keeps the title
					// centred over the cover.
					Row(verticalAlignment = Alignment.CenterVertically) {
						val ref = current.ref
						if (ref != null) Spacer(Modifier.size(48.dp))
						Text(
							text = current.title,
							style = MaterialTheme.typography.titleLarge,
							maxLines = 2,
							overflow = TextOverflow.Ellipsis,
							textAlign = TextAlign.Center,
							modifier = Modifier.weight(1f),
						)
						if (ref != null) {
							StarButton(
								ref = ref,
								kind = StarKind.SONG,
								fallback = current.starred,
								noun = "track",
							)
						}
					}
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

			item(key = "seek") {
				SeekBar(
					state,
					onSeek,
					onPlayPause = onTogglePlay,
					focusRequester = if (isTv) seekFocus else null,
				)
			}

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
					// A Row for the same reason the trailing one is: the
					// transport stays centred whatever this side adds up to.
					Row(
						modifier = Modifier
							.align(Alignment.CenterStart)
							.padding(start = 12.dp),
						verticalAlignment = Alignment.CenterVertically,
					) {
						// Always offered, unlike cast: it has no outcome that
						// is only a refusal, and what it answers - how the
						// audio is reaching the speaker, and in what format -
						// is nowhere else in the UI. On this side rather than
						// with the device controls, because three trailing
						// buttons ran into the transport on a phone.
						IconButton(onClick = onInfo) {
							Icon(
								Icons.Default.Info,
								contentDescription = "Track info",
								tint = MaterialTheme.colorScheme.onSurfaceVariant,
							)
						}
						// The way back to the picture after backing out of the
						// video screen, which leaves the sound playing. Without
						// it the only route back would be to start the film
						// again.
						if (state.isVideo) {
							IconButton(onClick = onWatch) {
								Icon(
									imageVector = Icons.Default.Movie,
									contentDescription = "Watch",
									tint = MaterialTheme.colorScheme.primary,
								)
							}
						}
					}
					// A Row rather than two aligned buttons, so the transport
					// stays centred whatever the trailing pair adds up to -
					// which is the same reason the Box above exists.
					Row(
						modifier = Modifier
							.align(Alignment.CenterEnd)
							.padding(end = 12.dp),
						verticalAlignment = Alignment.CenterVertically,
					) {
						// Only while playing locally, and in the slot the
						// speaker-controls button takes while casting: the slot
						// before cast always means controls for the device
						// making the sound, and exactly one of the two applies.
						if (!casting) {
							IconButton(onClick = onEqualizer) {
								Icon(
									Icons.Default.GraphicEq,
									contentDescription = "Equalizer",
									tint = MaterialTheme.colorScheme.onSurfaceVariant,
								)
							}
						}
						// Only while casting; which sheet it opens depends on
						// the device, and GainDriveApp decides. Placed before
						// the cast button so the one that is always there keeps
						// its position as this one comes and goes.
						if (casting) {
							IconButton(onClick = onSpeakerControls) {
								Icon(
									Icons.Default.Tune,
									contentDescription = "Speaker controls",
									tint = MaterialTheme.colorScheme.onSurfaceVariant,
								)
							}
						}
						// Offered for every video: one the server can only
						// re-encode is cast as HLS, which plays and seeks on a
						// receiver that fetches from the server itself. The one
						// case that still cannot - no direct route, so the
						// bridge would have to carry a playlist whose relative
						// segment URIs it cannot resolve - is refused in words
						// by PlayerConnection, since deciding it here would need
						// a reachability probe a composable cannot await.
						if (onCast != null) {
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
				itemsIndexed(rows, key = { _, row -> row.key }) { index, row ->
					val entry = row.entry
					val isCurrent = row.key == currentKey
					ReorderableItem(reorderState, key = row.key) { isDragging ->
						Surface(
							// Only while lifted: at rest the row sits on the sheet
							// like any other, and a surface of its own would show.
							color = if (isDragging) {
								MaterialTheme.colorScheme.surfaceContainerHigh
							} else {
								Color.Transparent
							},
							shadowElevation = if (isDragging) 4.dp else 0.dp,
						) {
							Row(
								modifier = Modifier
									.fillMaxWidth()
									.clickable { onJumpTo(index) }
									.padding(start = 24.dp, end = 8.dp, top = 8.dp, bottom = 8.dp),
								verticalAlignment = Alignment.CenterVertically,
								horizontalArrangement = Arrangement.spacedBy(12.dp),
							) {
								CoverThumb(entry.artworkUrl, entry.album, size = 32.dp)
								Column(modifier = Modifier.weight(1f)) {
									// Wraps, as the track rows in the listings do.
									Text(
										text = entry.title,
										style = MaterialTheme.typography.bodyMedium,
										color = if (isCurrent) {
											MaterialTheme.colorScheme.primary
										} else {
											MaterialTheme.colorScheme.onSurface
										},
									)
									Text(
										text = entry.artist,
										style = MaterialTheme.typography.bodySmall,
										color = MaterialTheme.colorScheme.onSurfaceVariant,
										maxLines = 1,
										overflow = TextOverflow.Ellipsis,
									)
								}
								// The track playing cannot be removed: dropping it
								// would mean deciding what plays instead, which is
								// what skip is for. It can be moved; Media3 keeps
								// it playing through that.
								if (!isCurrent) {
									IconButton(onClick = { onRemoveFromQueue(index) }) {
										Icon(
											Icons.Default.Close,
											contentDescription = "Remove from queue",
										)
									}
								}
								if (isTv) {
									// A remote cannot drag, so the move is a step
									// at a time.
									IconButton(
										onClick = { onMoveInQueue(index, index - 1) },
										enabled = index > 0,
									) {
										Icon(Icons.Default.KeyboardArrowUp, contentDescription = "Move up")
									}
									IconButton(
										onClick = { onMoveInQueue(index, index + 1) },
										enabled = index < rows.size - 1,
									) {
										Icon(Icons.Default.KeyboardArrowDown, contentDescription = "Move down")
									}
								} else {
									Icon(
										Icons.Default.DragHandle,
										contentDescription = "Reorder",
										tint = MaterialTheme.colorScheme.onSurfaceVariant,
										modifier = Modifier
											.draggableHandle(
												// Only the key is taken from the row: it
												// is the one thing about it that cannot
												// go stale (see latestPublished).
												onDragStarted = { dragging = row.key },
												onDragStopped = {
													val key = dragging
													dragging = null
													val from = latestPublished.indexOfFirst { it.key == key }
													val to = rows.indexOfFirst { it.key == key }
													if (from >= 0 && to >= 0 && to != from) {
														onMoveInQueue(from, to)
													} else {
														// Nothing to send, so no publish
														// will come to reset the rows.
														rows = published
													}
												},
											)
											.padding(12.dp),
									)
								}
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

/** One queue entry as drawn, under a key that survives reordering. */
private data class QueueRow(val key: String, val entry: NowPlaying)

/**
 * Keys the queue by ref plus occurrence, since the same track can be queued
 * twice and a lazy list refuses duplicate keys. Prefixed so none can collide
 * with the sheet's fixed items.
 */
private fun queueRows(queue: List<NowPlaying>): List<QueueRow> {
	val seen = mutableMapOf<String, Int>()
	return queue.map { entry ->
		val id = entry.ref?.encode() ?: entry.title
		val n = (seen[id] ?: 0) + 1
		seen[id] = n
		QueueRow("queue:$id#$n", entry)
	}
}

/** The title's row in the sheet's list: the cover is 0, the seek bar 2. */
private const val META_INDEX = 1

/** As in `claimsFocus`: lazy items are composed during layout, a frame late. */
private const val FOCUS_FRAMES = 10

private const val TAG = "GainDriveNowPlaying"
