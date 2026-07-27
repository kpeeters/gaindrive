package org.gaindrive.android.ui.components

import androidx.compose.foundation.clickable
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.width
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.dp
import org.gaindrive.android.data.model.Album
import org.gaindrive.android.data.model.Artist
import org.gaindrive.android.data.model.Song

/** Shared row composables. Every browse screen is built from these. */

@Composable
fun ArtistRow(artist: Artist, onClick: () -> Unit) {
	Row(
		modifier = Modifier
			.fillMaxWidth()
			.clickable(onClick = onClick)
			.padding(horizontal = 16.dp, vertical = 14.dp),
		verticalAlignment = Alignment.CenterVertically,
	) {
		Text(
			text = artist.name,
			style = MaterialTheme.typography.bodyLarge,
			maxLines = 1,
			overflow = TextOverflow.Ellipsis,
			modifier = Modifier.weight(1f),
		)
		Text(
			text = if (artist.albumCount == 1) "1 album" else "${artist.albumCount} albums",
			style = MaterialTheme.typography.bodySmall,
			color = MaterialTheme.colorScheme.onSurfaceVariant,
		)
	}
}

@Composable
fun AlbumRow(album: Album, coverUrl: String?, onClick: () -> Unit) {
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

/**
 * A track inside an album, where the number column keeps titles aligned.
 * [trailing] carries the per-track actions that arrive in sub-phase 2d.
 */
@Composable
fun TrackRow(
	song: Song,
	onClick: () -> Unit,
	modifier: Modifier = Modifier,
	showNumber: Boolean = true,
	trailing: @Composable (() -> Unit)? = null,
) {
	Row(
		modifier = modifier
			.fillMaxWidth()
			.clickable(onClick = onClick)
			.padding(horizontal = 16.dp, vertical = 12.dp),
		verticalAlignment = Alignment.CenterVertically,
		horizontalArrangement = Arrangement.spacedBy(12.dp),
	) {
		if (showNumber) {
			Text(
				text = song.track?.toString().orEmpty(),
				style = MaterialTheme.typography.bodySmall,
				color = MaterialTheme.colorScheme.onSurfaceVariant,
				modifier = Modifier.width(24.dp),
			)
		}
		Text(
			text = song.title,
			style = MaterialTheme.typography.bodyLarge,
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
@Composable
fun SongRow(
	song: Song,
	coverUrl: String?,
	onClick: () -> Unit,
	trailingText: String? = null,
) {
	Row(
		modifier = Modifier
			.fillMaxWidth()
			.clickable(onClick = onClick)
			.padding(horizontal = 16.dp, vertical = 8.dp),
		verticalAlignment = Alignment.CenterVertically,
		horizontalArrangement = Arrangement.spacedBy(12.dp),
	) {
		CoverThumb(coverUrl, song.albumTitle, size = 40.dp)
		Column(modifier = Modifier.weight(1f)) {
			Text(
				text = song.title,
				style = MaterialTheme.typography.bodyLarge,
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
		if (trailingText != null) {
			Text(
				text = trailingText,
				style = MaterialTheme.typography.bodySmall,
				color = MaterialTheme.colorScheme.onSurfaceVariant,
			)
		}
	}
}

fun formatDuration(seconds: Int): String {
	if (seconds <= 0) return ""
	val minutes = seconds / 60
	val remainder = seconds % 60
	return "%d:%02d".format(minutes, remainder)
}
