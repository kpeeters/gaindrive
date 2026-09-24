package org.gaindrive.android.ui.player

import androidx.compose.foundation.clickable
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.BoxWithConstraints
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.width
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.filled.Cast
import androidx.compose.material.icons.filled.CastConnected
import androidx.compose.material.icons.filled.Info
import androidx.compose.material.icons.filled.Pause
import androidx.compose.material.icons.filled.PlayArrow
import androidx.compose.material.icons.filled.SkipNext
import androidx.compose.material.icons.filled.SkipPrevious
import androidx.compose.material3.Icon
import androidx.compose.material3.IconButton
import androidx.compose.material3.LinearProgressIndicator
import androidx.compose.material3.LocalContentColor
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.dp
import org.gaindrive.android.playback.PlayerState
import org.gaindrive.android.ui.adaptive.paneCount
import org.gaindrive.android.ui.components.CoverThumb

/**
 * The persistent bar at the bottom of the content column. Sits in the app shell
 * rather than in the NavHost, so it survives navigation the way the web
 * client's fixed footer does - above the navigation bar on a phone, and beside
 * the rail on a tablet, which is where the web client's `#player` sits too.
 *
 * It draws no surface of its own and applies no window insets. Both belong to
 * the shell, which wraps this and the offline note in one surface and pads that
 * clear of the system bars - see the bottom bar in `GainDriveApp`. A second
 * tonal surface here would only double the tint.
 *
 * It has two forms, and it decides between them with [paneCount] - the same
 * threshold the pane strip uses, since "is there room for a second pane" and
 * "is there room for a real transport" are the same question and deserve one
 * answer. (Not quite the same *measurement*: the strip sits inside the
 * Scaffold's content insets and this bar does not, so at a window within a few
 * dp of the threshold, with a cutout or gesture inset in landscape, the two can
 * disagree by one pane. Cosmetic, and not worth a second measurement to fix.)
 *
 * A phone keeps the one-line row it always had, and reaches everything else
 * through the Now Playing sheet a tap away. Above that the bar grows what the
 * web client's own player bar has always carried: a previous button, the track
 * info and cast buttons, and the same [SeekBar] the sheet and the video screen
 * use. That is also what stops a phone-shaped row sitting in 1200dp of nothing.
 */
@Composable
fun MiniPlayer(
	state: PlayerState,
	onExpand: () -> Unit,
	onTogglePlay: () -> Unit,
	onNext: () -> Unit,
	onPrevious: () -> Unit,
	onSeek: (Long) -> Unit,
	casting: Boolean,
	onCast: (() -> Unit)?,
	onInfo: () -> Unit,
) {
	val current = state.current ?: return

	BoxWithConstraints {
		val wide = paneCount(maxWidth) > 1

		Column {
			// The thin line is the compact bar's only sense of position.
			// The wide one has a real scrub bar a few dp away, so drawing
			// both would be two readings of the same thing.
			if (!wide) {
				// Indeterminate while filling the buffer: a progress bar
				// frozen at zero looks like a stall rather than like work
				// in progress.
				if (state.isBuffering) {
					LinearProgressIndicator(modifier = Modifier.fillMaxWidth())
				} else {
					LinearProgressIndicator(
						progress = { progressOf(state) },
						modifier = Modifier.fillMaxWidth(),
					)
				}
			}
			Row(
				modifier = Modifier
					.fillMaxWidth()
					.clickable(onClick = onExpand)
					.padding(horizontal = 12.dp, vertical = 8.dp),
				verticalAlignment = Alignment.CenterVertically,
				horizontalArrangement = Arrangement.spacedBy(12.dp),
			) {
				CoverThumb(current.artworkUrl, current.album, size = 40.dp)

				Column(
					// Fixed once there is a scrub bar to share the row
					// with, so the bar gets the slack rather than the
					// title taking all of it.
					modifier = if (wide) {
						Modifier.width(WIDE_TITLE_WIDTH)
					} else {
						Modifier.weight(1f)
					},
				) {
					Text(
						text = current.title,
						style = MaterialTheme.typography.bodyMedium,
						maxLines = 1,
						overflow = TextOverflow.Ellipsis,
					)
					Text(
						text = current.artist,
						style = MaterialTheme.typography.bodySmall,
						color = MaterialTheme.colorScheme.onSurfaceVariant,
						maxLines = 1,
						overflow = TextOverflow.Ellipsis,
					)
				}

				if (wide) {
					IconButton(onClick = onPrevious, enabled = state.hasPrevious) {
						Icon(
							Icons.Default.SkipPrevious,
							contentDescription = "Previous",
						)
					}
				}
				IconButton(onClick = onTogglePlay) {
					Icon(
						imageVector = if (state.isPlaying) {
							Icons.Default.Pause
						} else {
							Icons.Default.PlayArrow
						},
						contentDescription = if (state.isPlaying) "Pause" else "Play",
					)
				}
				IconButton(onClick = onNext, enabled = state.hasNext) {
					Icon(Icons.Default.SkipNext, contentDescription = "Next")
				}

				// Info before cast, as in the Now Playing sheet and for the
				// same reason: cast comes and goes with what is playing, so
				// the button that is always there is the one that must not
				// move. The scrub bar after them takes up the slack, so
				// nothing to the left of it shifts either.
				if (wide) {
					IconButton(onClick = onInfo) {
						Icon(Icons.Default.Info, contentDescription = "Track info")
					}
					// Offered for every video now that a film the server can
					// only re-encode is cast as HLS. The one case left that
					// cannot be cast - no direct route to the server, so the
					// bridge would have to carry a playlist it cannot resolve -
					// is refused in words by PlayerConnection, because
					// answering it here would mean a reachability probe this
					// composable cannot await.
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
									LocalContentColor.current
								},
							)
						}
					}
				}

				if (wide) {
					SeekBar(
						state = state,
						onSeek = onSeek,
						// The row already pads its ends; the stock
						// modifier's own 24dp would double it.
						modifier = Modifier.weight(1f),
					)
				}
			}
		}
	}
}

/** Enough for a title and an artist, and no more - the rest is the scrub bar's. */
private val WIDE_TITLE_WIDTH = 220.dp

/** Zero rather than NaN before the player knows the duration. */
internal fun progressOf(state: PlayerState): Float =
	if (state.durationMs <= 0) 0f
	else (state.positionMs.toFloat() / state.durationMs).coerceIn(0f, 1f)
