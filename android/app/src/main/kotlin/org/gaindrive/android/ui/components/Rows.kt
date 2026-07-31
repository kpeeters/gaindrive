package org.gaindrive.android.ui.components

import androidx.compose.animation.core.animateFloatAsState
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
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.filled.DownloadDone
import androidx.compose.material3.CircularProgressIndicator
import androidx.compose.material3.Icon
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.getValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.alpha
import androidx.compose.ui.draw.clip
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.dp
import org.gaindrive.android.data.model.Album
import org.gaindrive.android.data.model.Artist
import org.gaindrive.android.data.model.ItemRef
import org.gaindrive.android.data.model.Playlist
import org.gaindrive.android.data.model.Song
import org.gaindrive.android.playback.TrackState
import org.gaindrive.android.ui.Availability
import org.gaindrive.android.ui.LocalAvailability

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
	val availability = LocalAvailability.current
	val playable = availability.of(song.ref) != Availability.UNAVAILABLE

	Row(
		modifier = modifier
			.fillMaxWidth()
			// Dimmed and inert rather than hidden: offline, seeing that a track
			// exists but is not here beats an album that has lost half its
			// tracks with no explanation.
			.alpha(if (playable) 1f else UNAVAILABLE_ALPHA)
			.combinedClickable(
				enabled = playable,
				onClick = onClick,
				onLongClick = onLongClick,
			)
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
		TrackDownloadMark(song.ref)
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
	val availability = LocalAvailability.current
	val playable = availability.of(song.ref) != Availability.UNAVAILABLE

	Row(
		modifier = Modifier
			.fillMaxWidth()
			.alpha(if (playable) 1f else UNAVAILABLE_ALPHA)
			.combinedClickable(
				enabled = playable,
				onClick = onClick,
				onLongClick = onLongClick,
			)
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
		TrackDownloadMark(song.ref)
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

/**
 * Where a track stands with respect to being downloaded: waiting its turn, being
 * fetched, or here.
 *
 * All three occupy the same 16dp so a row never reflows as an album downloads,
 * and nothing shows at all for a track that merely happens to be cached from
 * playing it — otherwise the mark would appear on everything recently played
 * and mean nothing.
 */
@Composable
private fun TrackDownloadMark(ref: ItemRef) {
	val availability = LocalAvailability.current
	val download = availability.downloadOf(ref)

	when {
		availability.isDownloaded(ref) -> Icon(
			imageVector = Icons.Default.DownloadDone,
			contentDescription = "Downloaded",
			tint = MaterialTheme.colorScheme.onSurfaceVariant,
			modifier = Modifier.size(16.dp),
		)

		download == null -> Unit

		// Queued, or fetching with no figure reported yet. An indeterminate
		// spinner is the honest picture: something is happening, and this track
		// is not the one it is happening to yet.
		download.knownFraction == null -> TrackSpinner()

		else -> {
			val fraction by animateFloatAsState(
				targetValue = download.knownFraction ?: 0f,
				label = "trackDownload",
			)
			CircularProgressIndicator(
				progress = { fraction },
				modifier = Modifier.size(16.dp),
				strokeWidth = 2.dp,
			)
		}
	}
}

/** Legible, but plainly not something you can tap. */
private const val UNAVAILABLE_ALPHA = 0.38f

/** Small enough to sit in a track number's place. */
@Composable
private fun TrackSpinner(tint: Color = MaterialTheme.colorScheme.primary) {
	CircularProgressIndicator(
		modifier = Modifier.size(16.dp),
		strokeWidth = 2.dp,
		color = tint,
	)
}

/** Whole numbers where they are exact, one decimal where they are not. */
fun formatBytes(bytes: Long): String {
	val gb = 1024.0 * 1024 * 1024
	val mb = 1024.0 * 1024
	return when {
		bytes >= gb -> {
			val value = bytes / gb
			if (value == value.toLong().toDouble()) "${value.toLong()} GB"
			else "%.1f GB".format(value)
		}
		bytes >= mb -> "%.0f MB".format(bytes / mb)
		else -> "${bytes / 1024} kB"
	}
}

fun formatDuration(seconds: Int): String {
	if (seconds <= 0) return ""
	val minutes = seconds / 60
	val remainder = seconds % 60
	return "%d:%02d".format(minutes, remainder)
}
