package org.gaindrive.android.ui.player

import androidx.compose.foundation.background
import androidx.compose.foundation.clickable
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.lazy.items
import androidx.compose.foundation.lazy.rememberLazyListState
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.filled.Close
import androidx.compose.material.icons.filled.KeyboardArrowDown
import androidx.compose.material.icons.filled.KeyboardArrowUp
import androidx.compose.material.icons.filled.PlayArrow
import androidx.compose.material3.HorizontalDivider
import androidx.compose.material3.Icon
import androidx.compose.material3.IconButton
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Surface
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.dp
import org.gaindrive.android.data.model.Chapter
import org.gaindrive.android.data.model.ChapterSource
import org.gaindrive.android.ui.components.formatChapterTime

/**
 * The markers inside the recording being played, over the picture.
 *
 * Over it rather than in a sheet, and opaque rather than tinted, for the same
 * two reasons the web client's panel is: jumping between the songs of a concert
 * is something you do *while watching it*, so the picture has to stay visible;
 * and text over a moving image is hard to read at any tint, so this is a panel
 * of the application that happens to sit over a video rather than an overlay
 * painted onto one. Themed, so it is not the one surface in the app that
 * ignores light mode.
 *
 * It does **not** hide with the transport. Those controls get out of the way
 * after a few seconds because they are in front of the film; this is a list
 * being read and scrolled, and having it vanish mid-scroll would make it
 * unusable. Its own close button and its toggle are what dismiss it.
 */
@Composable
fun ChapterPanel(
	chapters: List<Chapter>,
	source: ChapterSource,
	currentIndex: Int,
	onSeek: (Long) -> Unit,
	onPrevious: () -> Unit,
	onNext: () -> Unit,
	onClose: () -> Unit,
	modifier: Modifier = Modifier,
) {
	Surface(
		modifier = modifier,
		color = MaterialTheme.colorScheme.surface,
		contentColor = MaterialTheme.colorScheme.onSurface,
	) {
		Column {
			Row(
				modifier = Modifier
					.fillMaxWidth()
					.padding(horizontal = 4.dp, vertical = 4.dp),
				verticalAlignment = Alignment.CenterVertically,
			) {
				// Arrows rather than skip-previous/skip-next: those already mean
				// "the next item in the queue" a few dp away in the transport
				// row, and stepping between markers is not that.
				IconButton(onClick = onPrevious) {
					Icon(Icons.Default.KeyboardArrowUp, contentDescription = "Previous chapter")
				}
				IconButton(onClick = onNext) {
					Icon(Icons.Default.KeyboardArrowDown, contentDescription = "Next chapter")
				}
				Text(
					text = "Chapters",
					style = MaterialTheme.typography.titleSmall,
					modifier = Modifier.weight(1f),
				)
				IconButton(onClick = onClose) {
					Icon(Icons.Default.Close, contentDescription = "Close")
				}
			}
			HorizontalDivider()

			val listState = rememberLazyListState()
			// A concert can carry dozens of markers, so the one being played is
			// usually off-screen; without this the highlight is invisible for
			// most of the film.
			LaunchedEffect(currentIndex) {
				if (currentIndex >= 0) listState.animateScrollToItem(currentIndex)
			}

			LazyColumn(state = listState, modifier = Modifier.weight(1f)) {
				items(chapters, key = { it.index }) { chapter ->
					ChapterPanelRow(
						chapter = chapter,
						playing = chapters.getOrNull(currentIndex)?.index == chapter.index,
						onClick = { onSeek(chapter.startMs) },
					)
				}
			}

			// The one thing the source is worth saying out loud: these markers
			// are inside the file rather than in a sidecar beside it, which is
			// also why they do not appear in the album listing — that reads the
			// scan's index, and only sidecars are indexed.
			if (source == ChapterSource.CONTAINER) {
				HorizontalDivider()
				Text(
					text = "From the video file",
					style = MaterialTheme.typography.bodySmall,
					color = MaterialTheme.colorScheme.onSurfaceVariant,
					modifier = Modifier.padding(horizontal = 12.dp, vertical = 8.dp),
				)
			}
		}
	}
}

@Composable
private fun ChapterPanelRow(chapter: Chapter, playing: Boolean, onClick: () -> Unit) {
	Row(
		modifier = Modifier
			.fillMaxWidth()
			// The whole row, not just the icon: reading, there is nothing else a
			// tap on it could sensibly mean.
			.clickable(onClick = onClick)
			.background(
				if (playing) {
					MaterialTheme.colorScheme.surfaceVariant
				} else {
					MaterialTheme.colorScheme.surface
				}
			)
			.padding(horizontal = 8.dp, vertical = 10.dp),
		verticalAlignment = Alignment.CenterVertically,
		horizontalArrangement = Arrangement.spacedBy(8.dp),
	) {
		Icon(
			imageVector = Icons.Default.PlayArrow,
			contentDescription = null,
			tint = MaterialTheme.colorScheme.onSurfaceVariant,
			modifier = Modifier.size(16.dp),
		)
		Text(
			text = formatChapterTime(chapter.startSeconds),
			style = MaterialTheme.typography.bodySmall,
			color = MaterialTheme.colorScheme.onSurfaceVariant,
		)
		Text(
			text = chapter.displayName,
			style = MaterialTheme.typography.bodyMedium,
			color = if (playing) {
				MaterialTheme.colorScheme.primary
			} else {
				MaterialTheme.colorScheme.onSurface
			},
			maxLines = 1,
			overflow = TextOverflow.Ellipsis,
			modifier = Modifier.weight(1f),
		)
	}
}
