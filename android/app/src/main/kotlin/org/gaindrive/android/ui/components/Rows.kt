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
import androidx.compose.material.icons.filled.Circle
import androidx.compose.material.icons.filled.DownloadDone
import androidx.compose.material.icons.filled.Movie
import androidx.compose.material.icons.filled.PlayArrow
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
import org.gaindrive.android.data.model.ChapterHit
import org.gaindrive.android.data.model.ItemRef
import org.gaindrive.android.data.model.Playlist
import org.gaindrive.android.data.model.Song
import org.gaindrive.android.playback.TrackState
import org.gaindrive.android.ui.Availability
import org.gaindrive.android.ui.ContainerMark
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
			.heightIn(min = 40.dp)
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
		// Omitted rather than shown as zero, the same way an album row omits a
		// track count it was not given. Not every listing carries one: the
		// folder-browsing index does not count albums, and neither does search.
		if (artist.albumCount > 0) {
			Text(
				text = if (artist.albumCount == 1) "1 album" else "${artist.albumCount} albums",
				style = MaterialTheme.typography.bodySmall,
				color = MaterialTheme.colorScheme.onSurfaceVariant,
			)
		}
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
		ContainerDownloadMark(album.refs)
		AlbumVideoMark(album)
		ServerBadges(badges)
	}
}

/**
 * The album-list counterpart of [VideoMark], and the same glyph deliberately: a
 * folder of films and a film's one track are the same warning at two levels, and
 * two icons for it would read as two different things.
 *
 * Absent rather than zero on the listings the server does not carry the count
 * in — folder browsing, starred, search — so a missing mark is not a promise
 * that an album has no video. See `API-CLIENT.md`.
 */
@Composable
private fun AlbumVideoMark(album: Album) {
	if (album.videoCount <= 0) return
	Icon(
		imageVector = Icons.Default.Movie,
		contentDescription = if (album.videoCount == 1) "1 video"
			else "${album.videoCount} videos",
		tint = MaterialTheme.colorScheme.onSurfaceVariant,
		modifier = Modifier.size(16.dp),
	)
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
		ContainerDownloadMark(listOf(playlist.ref))
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
 *
 * [number] defaults to the song's own track number and exists so a caller
 * can substitute a positional one: an album ripped without tags reports
 * every track as 0, which the mapper folds to null, and a column of blanks
 * is worse than numbers nobody wrote down. The web client does the same.
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
	number: Int? = song.track,
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
			// The spinner and the playing mark take the number's place rather
			// than sitting beside it, so nothing in the row shifts when a track
			// starts loading or starts playing.
			Box(
				modifier = Modifier.width(24.dp),
				contentAlignment = Alignment.Center,
			) {
				when (playback) {
					TrackState.LOADING -> TrackSpinner()
					TrackState.CURRENT -> PlayingMark()
					TrackState.IDLE -> Text(
						text = number?.toString().orEmpty(),
						style = MaterialTheme.typography.bodySmall,
						color = MaterialTheme.colorScheme.onSurfaceVariant,
					)
				}
			}
		}
		// A Column holding one child lays out exactly as the bare Text did, so
		// a track whose artist is its album's is pixel-unchanged; the number,
		// the marks and the duration stay centred against whichever height it
		// comes to.
		Column(modifier = Modifier.weight(1f)) {
			// Wrapped rather than ellipsised: a row is free to grow, and what
			// a long title truncates away is usually the part that tells two
			// takes of one piece apart.
			Text(
				text = song.title,
				style = MaterialTheme.typography.bodyLarge,
				color = if (playback == TrackState.IDLE) {
					MaterialTheme.colorScheme.onSurface
				} else {
					MaterialTheme.colorScheme.primary
				},
			)
			// Only where it says something the album heading does not — a
			// guest, or every track of a compilation. The server decides what
			// counts as a difference; see Song.differingArtist.
			song.differingArtist?.let {
				Text(
					text = it,
					style = MaterialTheme.typography.bodySmall,
					color = MaterialTheme.colorScheme.onSurfaceVariant,
					maxLines = 1,
					overflow = TextOverflow.Ellipsis,
				)
			}
		}
		VideoMark(song)
		TrackDownloadMark(song.ref)
		Text(
			text = formatDuration(song.duration),
			style = MaterialTheme.typography.bodySmall,
			color = MaterialTheme.colorScheme.onSurfaceVariant,
		)
		trailing?.invoke()
	}
}

/**
 * Marks a row that will take over the screen when tapped.
 *
 * Videos sit in the same listings as tracks — a concert lives under its
 * performer, a film is an album with one track — so without this the only
 * warning is the film starting.
 */
@Composable
private fun VideoMark(song: Song) {
	if (!song.isVideo) return
	Icon(
		imageVector = Icons.Default.Movie,
		contentDescription = "Video",
		tint = MaterialTheme.colorScheme.onSurfaceVariant,
		modifier = Modifier.size(16.dp),
	)
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
		// There is no number column here, so the spinner and the playing mark
		// go over the artwork instead, again leaving the row's geometry
		// untouched.
		Box(contentAlignment = Alignment.Center) {
			CoverThumb(coverUrl, song.albumTitle, size = 40.dp)
			if (playback != TrackState.IDLE) {
				Box(
					modifier = Modifier
						.size(40.dp)
						.clip(RoundedCornerShape(4.dp))
						.background(MaterialTheme.colorScheme.scrim.copy(alpha = 0.5f)),
					contentAlignment = Alignment.Center,
				) {
					// Both are tinted against the scrim rather than accent,
					// because they lie over arbitrary artwork and red on dark is
					// the one pairing that can vanish. The title beside them
					// carries the red.
					if (playback == TrackState.LOADING) {
						TrackSpinner(tint = MaterialTheme.colorScheme.inverseOnSurface)
					} else {
						PlayingMark(tint = MaterialTheme.colorScheme.inverseOnSurface)
					}
				}
			}
		}
		Column(modifier = Modifier.weight(1f)) {
			// Wraps, as in [TrackRow]. The line below it does not: an artist and
			// album are context, and two of them wrapping would bury the title.
			Text(
				text = song.title,
				style = MaterialTheme.typography.bodyLarge,
				color = if (playback == TrackState.IDLE) {
					MaterialTheme.colorScheme.onSurface
				} else {
					MaterialTheme.colorScheme.primary
				},
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
		VideoMark(song)
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
 * One marker inside a recording, drawn as a row of the album it sits in.
 *
 * Shaped like [TrackRow] — the same number box, the same title weight, the same
 * trailing duration — because in a listing that is exactly what it stands for:
 * a chaptered concert's markers replace its single row, so they have to read as
 * the album's tracks and not as an annotation on one.
 *
 * What it deliberately lacks is everything that addresses a song id. There is
 * no star, no download mark and no video mark, because a chapter has no id of
 * its own to star, download or play on its own. Its long-press reaches the
 * recording's actions instead, which is the only handle on the file left once
 * its own row has been replaced.
 *
 * Dimmed until it is the marker being played, so the list reads as positions
 * inside one recording rather than as tracks in their own right.
 */
@OptIn(ExperimentalFoundationApi::class)
@Composable
fun ChapterRow(
	number: Int,
	title: String,
	duration: Int,
	playing: Boolean,
	onClick: () -> Unit,
	onLongClick: (() -> Unit)? = null,
) {
	Row(
		modifier = Modifier
			.fillMaxWidth()
			.combinedClickable(onClick = onClick, onLongClick = onLongClick)
			.padding(horizontal = 16.dp, vertical = 12.dp),
		verticalAlignment = Alignment.CenterVertically,
		horizontalArrangement = Arrangement.spacedBy(12.dp),
	) {
		// The mark takes the number's place, as it does in [TrackRow].
		Box(modifier = Modifier.width(24.dp), contentAlignment = Alignment.Center) {
			if (playing) {
				PlayingMark()
			} else {
				Text(
					text = number.toString(),
					style = MaterialTheme.typography.bodySmall,
					color = MaterialTheme.colorScheme.onSurfaceVariant,
				)
			}
		}
		Text(
			text = title,
			style = MaterialTheme.typography.bodyLarge,
			color = if (playing) {
				MaterialTheme.colorScheme.primary
			} else {
				MaterialTheme.colorScheme.onSurfaceVariant
			},
			modifier = Modifier.weight(1f),
		)
		Text(
			text = formatDuration(duration),
			style = MaterialTheme.typography.bodySmall,
			color = MaterialTheme.colorScheme.onSurfaceVariant,
		)
	}
}

/**
 * A chapter match in a search listing.
 *
 * Shaped like [SongRow] minus the artwork, which a marker has none of — its
 * recording's cover is the album's, and drawing it on every row would say the
 * hits were albums.
 *
 * The trailing column carries *where in the recording* the marker is, where a
 * song row carries how long it is. That is the one fact a search hit has and a
 * listing row does not, and it is what tells two takes of one song apart.
 */
@Composable
fun ChapterHitRow(hit: ChapterHit, onClick: () -> Unit) {
	Row(
		modifier = Modifier
			.fillMaxWidth()
			.clickable(onClick = onClick)
			.padding(horizontal = 16.dp, vertical = 8.dp),
		verticalAlignment = Alignment.CenterVertically,
		horizontalArrangement = Arrangement.spacedBy(12.dp),
	) {
		Column(modifier = Modifier.weight(1f)) {
			Text(
				text = hit.displayName,
				style = MaterialTheme.typography.bodyLarge,
				color = MaterialTheme.colorScheme.onSurface,
			)
			Text(
				// Artist, album, then the recording itself: a marker means
				// nothing without knowing which concert it is in. The album is
				// what the folder is called and the track what the file is
				// called, and they coincide often enough — a folder holding one
				// recording named after it — that an exact duplicate is dropped
				// rather than printed twice.
				text = buildList {
					add(hit.artistName)
					add(hit.albumTitle)
					if (hit.trackTitle != hit.albumTitle) add(hit.trackTitle)
				}.filter { it.isNotBlank() }.joinToString(" · "),
				style = MaterialTheme.typography.bodySmall,
				color = MaterialTheme.colorScheme.onSurfaceVariant,
				maxLines = 1,
				overflow = TextOverflow.Ellipsis,
			)
		}
		Text(
			text = formatChapterTime(hit.startSeconds),
			style = MaterialTheme.typography.bodySmall,
			color = MaterialTheme.colorScheme.onSurfaceVariant,
		)
	}
}

/**
 * Where a whole album or playlist stands, in the same two marks its tracks use.
 *
 * A tick means downloaded — asked for, and safe from eviction — and before it
 * the ring, clock or error the album's own screen shows, so a row never
 * disagrees with the screen it opens. A dot means every track is here without
 * anything keeping it that way, which is what a record played straight through
 * looks like.
 *
 * Nothing at all is drawn otherwise, and that includes an album whose tracks
 * the mirror has never seen. It deliberately does **not** fall back to
 * [DownloadIndicator]'s own null state: that is a download arrow meaning "not
 * yet", which is an invitation on a button and, down the side of a list, a
 * column of buttons that do nothing.
 */
@Composable
private fun ContainerDownloadMark(refs: List<ItemRef>) {
	when (val mark = LocalAvailability.current.containerMark(refs)) {
		null -> Unit

		ContainerMark.StoredOnly -> Icon(
			imageVector = Icons.Default.Circle,
			contentDescription = "Stored, but not downloaded",
			tint = MaterialTheme.colorScheme.onSurfaceVariant,
			modifier = Modifier.size(8.dp),
		)

		// Both sizes given, or the running ring keeps its own and the row
		// reflows the moment a download finishes.
		is ContainerMark.Pinned -> DownloadIndicator(
			status = mark.status,
			modifier = Modifier.size(MARK_SIZE),
			ringSize = MARK_SIZE,
		)
	}
}

/**
 * Where a track stands with respect to being downloaded: waiting its turn, being
 * fetched, or here.
 *
 * Two kinds of "here", deliberately distinct. A tick means downloaded — asked
 * for, and safe from eviction. A small dot means merely cached from having been
 * played, which will still play offline but may be reclaimed when the cache
 * fills. Collapsing them would promise permanence the second kind does not have.
 *
 * Everything occupies at most 16dp so a row never reflows as an album
 * downloads.
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

		// Cached by being played rather than downloaded on purpose. A smaller,
		// quieter mark: it says the track will play offline, without claiming
		// the permanence a download has — eviction may take it back.
		download == null && availability.isCachedOnly(ref) -> Icon(
			imageVector = Icons.Default.Circle,
			contentDescription = "Stored, but not downloaded",
			tint = MaterialTheme.colorScheme.onSurfaceVariant,
			modifier = Modifier.size(8.dp),
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

/** What every download mark occupies, so no row reflows as one progresses. */
private val MARK_SIZE = 16.dp

/** Small enough to sit in a track number's place. */
@Composable
private fun TrackSpinner(tint: Color = MaterialTheme.colorScheme.primary) {
	CircularProgressIndicator(
		modifier = Modifier.size(16.dp),
		strokeWidth = 2.dp,
		color = tint,
	)
}

/**
 * Marks the row the player is on, as the web client's filled triangle does.
 *
 * Sized and tinted like [TrackSpinner] because it stands in the same place and
 * is the state that follows it: a track loads, then it plays, and neither may
 * move the row it is in. It carries a content description because the red title
 * beside it says nothing to a screen reader, and says little to an eye that
 * does not separate the two colours.
 */
@Composable
private fun PlayingMark(tint: Color = MaterialTheme.colorScheme.primary) {
	Icon(
		imageVector = Icons.Default.PlayArrow,
		contentDescription = "Playing",
		tint = tint,
		modifier = Modifier.size(16.dp),
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

/**
 * A *position* inside a recording: `H:MM:SS` past an hour, `M:SS` below it.
 *
 * [formatDuration] is not reusable here, twice over. It has no hour rollover,
 * so it prints 5400 seconds as "90:00" — unreadable as a place in a two-hour
 * concert. And it returns "" for zero, which is right for a length and wrong
 * for a position: 0:00 is a real one, and the first marker is usually at it.
 *
 * Floors rather than rounds, matching `fmtChapterTime` in `web/app.js`, so the
 * two clients name the same second for the same marker.
 */
fun formatChapterTime(seconds: Double): String {
	val total = seconds.coerceAtLeast(0.0).toLong()
	val hours = total / 3600
	val minutes = (total % 3600) / 60
	val remainder = total % 60
	return if (hours > 0) {
		"%d:%02d:%02d".format(hours, minutes, remainder)
	} else {
		"%d:%02d".format(minutes, remainder)
	}
}
