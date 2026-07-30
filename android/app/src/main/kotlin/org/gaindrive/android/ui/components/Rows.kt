package org.gaindrive.android.ui.components

import androidx.compose.foundation.ExperimentalFoundationApi
import androidx.compose.foundation.background
import androidx.compose.foundation.clickable
import androidx.compose.foundation.combinedClickable
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.heightIn
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.material3.CircularProgressIndicator
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clip
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.dp
import org.gaindrive.android.data.model.Album
import org.gaindrive.android.data.model.Artist
import org.gaindrive.android.data.model.Playlist
import org.gaindrive.android.data.model.Song
import org.gaindrive.android.playback.TrackState

/** Shared row composables. Every browse screen is built from these. */

/** Groups rows under a category or a server name. */
@Composable
fun SectionHeading(text: String) {
	Text(
		text = text,
		style = MaterialTheme.typography.titleSmall,
		color = MaterialTheme.colorScheme.primary,
		modifier = Modifier.padding(start = 16.dp, top = 16.dp, bottom = 4.dp),
	)
}

@Composable
fun ArtistRow(artist: Artist, onClick: () -> Unit, badges: List<String> = emptyList()) {
	Row(
		modifier = Modifier
			.fillMaxWidth()
			// heightIn rather than generous padding: it holds the 48dp minimum
			// touch target even at large font scales, while letting the rows sit
			// close together. There is no divider between them — with rows this
			// dense, one line per artist reads better than a ruled list.
			.heightIn(min = 48.dp)
			.clickable(onClick = onClick)
			.padding(horizontal = 16.dp, vertical = 4.dp),
		verticalAlignment = Alignment.CenterVertically,
		horizontalArrangement = Arrangement.spacedBy(8.dp),
	) {
		Text(
			text = artist.name,
			style = MaterialTheme.typography.bodyLarge,
			maxLines = 1,
			overflow = TextOverflow.Ellipsis,
			modifier = Modifier.weight(1f),
		)
		// Several badges mean this one row is several servers' artists, which
		// is worth seeing before tapping into a merged album list.
		ServerBadges(badges)
		Text(
			text = if (artist.albumCount == 1) "1 album" else "${artist.albumCount} albums",
			style = MaterialTheme.typography.bodySmall,
			color = MaterialTheme.colorScheme.onSurfaceVariant,
		)
	}
}

@Composable
fun AlbumRow(
	album: Album,
	coverUrl: String?,
	onClick: () -> Unit,
	badges: List<String> = emptyList(),
) {
	Row(
		modifier = Modifier
			.fillMaxWidth()
			.clickable(onClick = onClick)
			.padding(horizontal = 16.dp, vertical = 8.dp),
		verticalAlignment = Alignment.CenterVertically,
		horizontalArrangement = Arrangement.spacedBy(12.dp),
	) {
		CoverThumb(coverUrl, album.title)
		Column(modifier = Modifier.weight(1f)) {
			Text(
				text = album.title,
				style = MaterialTheme.typography.bodyLarge,
				maxLines = 1,
				overflow = TextOverflow.Ellipsis,
			)
			Text(
				text = albumSubtitle(album),
				style = MaterialTheme.typography.bodySmall,
				color = MaterialTheme.colorScheme.onSurfaceVariant,
				maxLines = 1,
				overflow = TextOverflow.Ellipsis,
			)
		}
		ServerBadges(badges)
	}
}

private fun albumSubtitle(album: Album): String {
	val parts = buildList {
		album.year?.let { add(it.toString()) }
		if (album.songCount > 0) {
			add(if (album.songCount == 1) "1 track" else "${album.songCount} tracks")
		}
	}
	return parts.joinToString(" · ")
}

/** [trailing] carries per-row actions, e.g. "remove from playlist". */
@Composable
fun PlaylistRow(
	playlist: Playlist,
	onClick: () -> Unit,
	trailing: @Composable (() -> Unit)? = null,
) {
	Row(
		modifier = Modifier
			.fillMaxWidth()
			.heightIn(min = 56.dp)
			.clickable(onClick = onClick)
			.padding(start = 16.dp, top = 8.dp, bottom = 8.dp),
		verticalAlignment = Alignment.CenterVertically,
		horizontalArrangement = Arrangement.spacedBy(12.dp),
	) {
		Column(modifier = Modifier.weight(1f)) {
			Text(
				text = playlist.name,
				style = MaterialTheme.typography.bodyLarge,
				maxLines = 1,
				overflow = TextOverflow.Ellipsis,
			)
			Text(
				text = playlistSubtitle(playlist),
				style = MaterialTheme.typography.bodySmall,
				color = MaterialTheme.colorScheme.onSurfaceVariant,
				maxLines = 1,
				overflow = TextOverflow.Ellipsis,
			)
		}
		trailing?.invoke()
	}
}

/**
 * Hours and minutes rather than [formatDuration]'s mm:ss — a playlist runs for
 * hours, and "184:07" is not a length anyone reads.
 */
private fun playlistSubtitle(playlist: Playlist): String {
	val tracks =
		if (playlist.songCount == 1) "1 track" else "${playlist.songCount} tracks"
	val minutes = playlist.duration / 60
	return when {
		minutes <= 0 -> tracks
		minutes < 60 -> "$tracks · $minutes min"
		else -> "$tracks · ${minutes / 60} h ${minutes % 60} min"
	}
}

/**
 * A track inside an album or playlist, where the number column keeps titles
 * aligned. [trailing] carries the per-track actions.
 */
@OptIn(ExperimentalFoundationApi::class)
@Composable
fun TrackRow(
	song: Song,
	onClick: () -> Unit,
	modifier: Modifier = Modifier,
	onLongClick: (() -> Unit)? = null,
	playback: TrackState = TrackState.IDLE,
	showNumber: Boolean = true,
	trailing: @Composable (() -> Unit)? = null,
) {
	Row(
		modifier = modifier
			.fillMaxWidth()
			.combinedClickable(onClick = onClick, onLongClick = onLongClick)
			.padding(horizontal = 16.dp, vertical = 12.dp),
		verticalAlignment = Alignment.CenterVertically,
		horizontalArrangement = Arrangement.spacedBy(12.dp),
	) {
		if (showNumber) {
			// The spinner takes the number's place rather than sitting beside
			// it, so nothing in the row shifts when a track starts loading.
			Box(
				modifier = Modifier.width(24.dp),
				contentAlignment = Alignment.Center,
			) {
				if (playback == TrackState.LOADING) {
					TrackSpinner()
				} else {
					Text(
						text = song.track?.toString().orEmpty(),
						style = MaterialTheme.typography.bodySmall,
						color = MaterialTheme.colorScheme.onSurfaceVariant,
					)
				}
			}
		}
		Text(
			text = song.title,
			style = MaterialTheme.typography.bodyLarge,
			color = if (playback == TrackState.IDLE) {
				MaterialTheme.colorScheme.onSurface
			} else {
				MaterialTheme.colorScheme.primary
			},
			maxLines = 1,
			overflow = TextOverflow.Ellipsis,
			modifier = Modifier.weight(1f),
		)
		Text(
			text = formatDuration(song.duration),
			style = MaterialTheme.typography.bodySmall,
			color = MaterialTheme.colorScheme.onSurfaceVariant,
		)
		trailing?.invoke()
	}
}

/** A song shown outside its album, so it needs artist and album for context. */
@OptIn(ExperimentalFoundationApi::class)
@Composable
fun SongRow(
	song: Song,
	coverUrl: String?,
	onClick: () -> Unit,
	onLongClick: (() -> Unit)? = null,
	playback: TrackState = TrackState.IDLE,
	trailingText: String? = null,
	badge: String? = null,
	trailing: @Composable (() -> Unit)? = null,
) {
	Row(
		modifier = Modifier
			.fillMaxWidth()
			.combinedClickable(onClick = onClick, onLongClick = onLongClick)
			.padding(horizontal = 16.dp, vertical = 8.dp),
		verticalAlignment = Alignment.CenterVertically,
		horizontalArrangement = Arrangement.spacedBy(12.dp),
	) {
		// There is no number column here, so the spinner goes over the artwork
		// — again leaving the row's geometry untouched.
		Box(contentAlignment = Alignment.Center) {
			CoverThumb(coverUrl, song.albumTitle, size = 40.dp)
			if (playback == TrackState.LOADING) {
				Box(
					modifier = Modifier
						.size(40.dp)
						.clip(RoundedCornerShape(4.dp))
						.background(MaterialTheme.colorScheme.scrim.copy(alpha = 0.5f)),
					contentAlignment = Alignment.Center,
				) {
					TrackSpinner(tint = MaterialTheme.colorScheme.inverseOnSurface)
				}
			}
		}
		Column(modifier = Modifier.weight(1f)) {
			Text(
				text = song.title,
				style = MaterialTheme.typography.bodyLarge,
				color = if (playback == TrackState.IDLE) {
					MaterialTheme.colorScheme.onSurface
				} else {
					MaterialTheme.colorScheme.primary
				},
				maxLines = 1,
				overflow = TextOverflow.Ellipsis,
			)
			Text(
				text = listOf(song.artistName, song.albumTitle)
					.filter { it.isNotBlank() }
					.joinToString(" · "),
				style = MaterialTheme.typography.bodySmall,
				color = MaterialTheme.colorScheme.onSurfaceVariant,
				maxLines = 1,
				overflow = TextOverflow.Ellipsis,
			)
		}
		ServerBadge(badge)
		if (trailingText != null) {
			Text(
				text = trailingText,
				style = MaterialTheme.typography.bodySmall,
				color = MaterialTheme.colorScheme.onSurfaceVariant,
			)
		}
		trailing?.invoke()
	}
}

/** Small enough to sit in a track number's place. */
@Composable
private fun TrackSpinner(tint: Color = MaterialTheme.colorScheme.primary) {
	CircularProgressIndicator(
		modifier = Modifier.size(16.dp),
		strokeWidth = 2.dp,
		color = tint,
	)
}

fun formatDuration(seconds: Int): String {
	if (seconds <= 0) return ""
	val minutes = seconds / 60
	val remainder = seconds % 60
	return "%d:%02d".format(minutes, remainder)
}
