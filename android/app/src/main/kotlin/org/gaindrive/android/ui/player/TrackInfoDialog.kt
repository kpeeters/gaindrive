package org.gaindrive.android.ui.player

import android.content.Intent
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.verticalScroll
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.filled.Share
import androidx.compose.material3.AlertDialog
import androidx.compose.material3.Checkbox
import androidx.compose.material3.Icon
import androidx.compose.material3.IconButton
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Text
import androidx.compose.material3.TextButton
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.dp
import androidx.hilt.navigation.compose.hiltViewModel
import androidx.lifecycle.compose.collectAsStateWithLifecycle
import org.gaindrive.android.data.chapterAt
import org.gaindrive.android.data.model.AudioFormat
import org.gaindrive.android.data.model.AudioQuality
import org.gaindrive.android.data.model.Chapter
import org.gaindrive.android.data.model.ItemRef
import org.gaindrive.android.data.model.Song
import org.gaindrive.android.data.trackShareUrl
import org.gaindrive.android.playback.NowPlaying
import org.gaindrive.android.playback.demuxedLocally
import org.gaindrive.android.playback.cast.CastMedia
import org.gaindrive.android.playback.cast.CastRoute
import org.gaindrive.android.ui.components.formatDuration

/**
 * The equivalent of the web client's player-bar info modal, extended to answer
 * the two questions casting raises and nothing else could: **who is serving the
 * bytes**, and **in what format**.
 *
 * Neither is recoverable after the fact. The route is decided per track in
 * `CastUrls.forCast`, and the receiver reports no codec and no bitrate of its
 * own — the `contentType` it echoes is only what the `LOAD` told it. So both
 * are read from what the phone recorded when it built the load, not from the
 * receiver.
 *
 * **The stream URL is deliberately not shown.** It carries the account's
 * `t=`/`s=` auth parameters, and the bridge's session token, neither of which
 * should reach a screenshot; the server's *name* answers the question the URL
 * would have.
 */
@Composable
fun TrackInfoDialog(
	current: NowPlaying,
	casting: Boolean,
	positionMs: Long,
	onDismiss: () -> Unit,
	viewModel: TrackInfoViewModel = hiltViewModel(),
) {
	val state by viewModel.state.collectAsStateWithLifecycle()
	val loaded by viewModel.loaded.collectAsStateWithLifecycle()
	val device by viewModel.device.collectAsStateWithLifecycle()

	// Keyed on the track: the sheet stays open across a queue advance, and the
	// dialog opened on one track must not go on describing it.
	LaunchedEffect(current.ref) {
		current.ref?.let(viewModel::load)
	}

	AlertDialog(
		onDismissRequest = onDismiss,
		title = { Text("Track info") },
		text = {
			Column(
				modifier = Modifier.verticalScroll(rememberScrollState()),
				verticalArrangement = Arrangement.spacedBy(4.dp),
			) {
				TrackRows(current, state.song)
				Heading("Playback")
				PlaybackRows(
					current = current,
					song = state.song,
					casting = casting,
					deviceName = device?.name,
					serverName = state.serverName,
					loaded = loaded,
				)
				// Unlike the stream URL the KDoc above rules out, this link is
				// safe to show: it names the server and a track id and carries
				// no credentials by construction.
				val serverUrl = state.serverUrl
				val ref = current.ref
				if (ref != null && serverUrl != null) {
					Heading("Share")
					ShareSection(
						ref = ref,
						serverUrl = serverUrl,
						chapters = state.chapters,
						positionMs = positionMs,
					)
				}
			}
		},
		confirmButton = {
			TextButton(onClick = onDismiss) { Text("Close") }
		},
	)
}

@Composable
private fun TrackRows(current: NowPlaying, song: Song?) {
	// The queue entry is the fallback throughout: it is what is playing, and
	// the mirror may not hold this track at all.
	InfoRow("Title", song?.title ?: current.title)
	InfoRow("Artist", song?.artistName ?: current.artist)
	InfoRow("Album", song?.albumTitle ?: current.album)
	InfoRow("Track", song?.track?.toString())
	InfoRow("Year", song?.year?.toString())
	InfoRow("Length", song?.duration?.let(::formatDuration))
	InfoRow("File", song?.let(::fileLabel))
	InfoRow("Starred", song?.let { if (it.isStarred) "Yes" else "No" })
}

@Composable
private fun PlaybackRows(
	current: NowPlaying,
	song: Song?,
	casting: Boolean,
	deviceName: String?,
	serverName: String?,
	loaded: CastMedia?,
) {
	InfoRow(
		"Output",
		if (casting) "Chromecast${deviceName?.let { " “$it”" } ?: ""}" else "This device",
	)

	// Only while casting, and only once a LOAD has been built: connecting to a
	// device does not by itself decide anything, and a route named before it
	// was chosen would be a guess.
	if (casting) {
		InfoRow("Route", loaded?.route?.let { routeLabel(it, serverName) })
	}

	// While casting the receiver's copy is the one that matters, and it is
	// resolved separately from the local player's — a queue may have been
	// playing locally at a quality the cast path then re-decided.
	val quality = if (casting) loaded?.quality else current.quality
	InfoRow("Sent", sentLabel(current, song, quality, casting))

	if (casting) {
		InfoRow(
			"Declared type",
			loaded?.let { it.mimeType ?: "Not declared — the player sniffs it" },
		)
	}
}

/** What is on the server: the container and the bitrate it was stored at. */
private fun fileLabel(song: Song): String? {
	val suffix = song.suffix?.uppercase()
	val rate = song.bitRate.takeIf { it > 0 }?.let { "$it kbps" }
	return listOfNotNull(suffix, rate).takeIf { it.isNotEmpty() }?.joinToString(" · ")
}

/**
 * What the server was asked for.
 *
 * Video has no such choice — `format` and `maxBitRate` are never sent, because
 * either one demotes a file that could have been served off disk — so its
 * answer comes from `nativeSeek`, which is the same flag `StreamUrls.forVideo`
 * branches on and therefore cannot drift from what actually happens.
 *
 * Within `nativeSeek` there are still two tiers, and which one this playback
 * got is not a property of the file alone: local playback declares the
 * containers media3 demuxes and is handed those untouched, while the cast route
 * declares nothing and takes the remux. Hence [casting] — the same track can
 * honestly answer this differently depending on who is reading the bytes.
 */
private fun sentLabel(
	current: NowPlaying,
	song: Song?,
	quality: AudioQuality?,
	casting: Boolean,
): String? = when {
	current.isVideo && current.nativeSeek ->
		// Untouched either because this container goes to every client as it
		// stands, or because we told the server we demux this one ourselves.
		if (song?.transcodedContentType == null ||
			(!casting && demuxedLocally(song.suffix))
		) "As stored" else "Remuxed to MP4"
	current.isVideo -> "HLS, re-encoded as it plays"
	quality == null -> null
	quality.format == AudioFormat.ORIGINAL ->
		listOfNotNull(song?.suffix?.uppercase(), "unconverted").joinToString(", ")
	else -> "${quality.label} kbps"
}

private fun routeLabel(route: CastRoute, serverName: String?): String {
	val server = serverName ?: "the server"
	return when (route) {
		CastRoute.DIRECT -> "Direct — the player fetches from $server"
		CastRoute.RELAY -> "Through this phone — relayed from $server"
		CastRoute.LOCAL -> "Through this phone — from the downloaded copy"
	}
}

/**
 * The web dialog's Share section, with the platform's own ending: no copy
 * button, a share icon firing the standard sheet — which itself offers copy.
 * Same rules otherwise: the position is a snapshot taken as the dialog opens
 * (PlayerState ticks twice a second, and a label that crept on would name
 * some other moment by the time it was ticked); the chapter is the marker
 * under that snapshot; the two checkboxes both answer "where should this
 * link start", so ticking either lets go of the other; a position of 0 and a
 * chapter starting at 0 offer nothing and are not drawn.
 */
@Composable
private fun ShareSection(
	ref: ItemRef,
	serverUrl: String,
	chapters: List<Chapter>,
	positionMs: Long,
) {
	val snapshotSec = remember(ref) { positionMs / 1000 }
	val chapter = chapterAt(chapters, snapshotSec)
	var atPosition by remember(ref) { mutableStateOf(false) }
	var atChapter by remember(ref) { mutableStateOf(false) }

	val t = when {
		atChapter && chapter != null -> chapter.startSeconds
		atPosition && snapshotSec > 0 -> snapshotSec.toDouble()
		else -> 0.0
	}
	// The visible text is the exact string shared, so what the sheet sends
	// is never a surprise.
	val url = trackShareUrl(serverUrl, ref.id, t)

	val context = LocalContext.current
	Row(
		modifier = Modifier.fillMaxWidth(),
		verticalAlignment = Alignment.CenterVertically,
	) {
		Text(
			text = url,
			style = MaterialTheme.typography.bodySmall,
			color = MaterialTheme.colorScheme.onSurfaceVariant,
			maxLines = 1,
			overflow = TextOverflow.Ellipsis,
			modifier = Modifier.weight(1f),
		)
		IconButton(onClick = {
			val send = Intent(Intent.ACTION_SEND)
				.setType("text/plain")
				.putExtra(Intent.EXTRA_TEXT, url)
			context.startActivity(Intent.createChooser(send, null))
		}) {
			Icon(Icons.Default.Share, contentDescription = "Share link")
		}
	}

	if (snapshotSec > 0) {
		ShareToggle(
			label = "Start at ${formatDuration(snapshotSec.toInt())}",
			checked = atPosition,
		) { checked ->
			atPosition = checked
			if (checked) atChapter = false
		}
	}
	if (chapter != null) {
		ShareToggle(
			label = "Start at chapter ${chapter.displayName}",
			checked = atChapter,
		) { checked ->
			atChapter = checked
			if (checked) atPosition = false
		}
	}
}

@Composable
private fun ShareToggle(label: String, checked: Boolean, onChange: (Boolean) -> Unit) {
	Row(
		modifier = Modifier.fillMaxWidth(),
		verticalAlignment = Alignment.CenterVertically,
	) {
		Checkbox(checked = checked, onCheckedChange = onChange)
		Text(
			text = label,
			style = MaterialTheme.typography.bodyMedium,
			maxLines = 1,
			overflow = TextOverflow.Ellipsis,
		)
	}
}

@Composable
private fun Heading(text: String) {
	Text(
		text = text,
		style = MaterialTheme.typography.titleSmall,
		color = MaterialTheme.colorScheme.primary,
		modifier = Modifier.padding(top = 12.dp, bottom = 4.dp),
	)
}

/**
 * One label/value pair, in the shape of the web modal's `dt`/`dd` grid. A row
 * with nothing to say is not drawn at all, which is the same rule that modal
 * follows — a dialog of empty labels tells the reader less than a short one.
 */
@Composable
private fun InfoRow(label: String, value: String?) {
	if (value.isNullOrBlank()) return
	Row(modifier = Modifier.fillMaxWidth(), verticalAlignment = Alignment.Top) {
		Text(
			text = label,
			style = MaterialTheme.typography.bodySmall,
			color = MaterialTheme.colorScheme.onSurfaceVariant,
			modifier = Modifier.width(104.dp),
		)
		Text(
			text = value,
			style = MaterialTheme.typography.bodyMedium,
			modifier = Modifier.weight(1f),
		)
	}
}
