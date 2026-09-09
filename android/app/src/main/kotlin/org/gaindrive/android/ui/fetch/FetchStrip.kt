package org.gaindrive.android.ui.fetch

import androidx.compose.foundation.clickable
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.filled.AddLink
import androidx.compose.material.icons.filled.Close
import androidx.compose.material3.Icon
import androidx.compose.material3.IconButton
import androidx.compose.material3.LinearProgressIndicator
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Surface
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.dp
import org.gaindrive.android.data.FetchJobRef
import org.gaindrive.android.data.model.FetchState

/**
 * A fetch running on a server, reported from wherever the user happens to be.
 *
 * **It sits with the player, for the reason `OfflineNote` beside it gives**: a
 * fetch in progress is a fact about the whole app rather than about one screen,
 * and one strip is better than five that have to agree. Being outside the
 * `NavHost` is the substantive part — a fetch outlives the panel that started
 * it, and the panel's poll dying on navigation is exactly how someone came to
 * fetch the same URL twice.
 *
 * Above the player rather than below it, so the transport stays anchored to the
 * bottom edge and does not jump under a thumb when a fetch starts or ends.
 *
 * Never drawn over the fetch panel itself: the shell hides this whole bar on
 * `Route.FetchUrl`, which reports the same jobs in more detail.
 */
@Composable
fun FetchStrip(
	state: FetchStripState,
	onOpen: () -> Unit,
	onDismiss: (String) -> Unit,
) {
	val live = state.live
	val moving = state.moving
	val notice = state.notice
	if (live.isEmpty() && notice == null) return

	Surface(
		color = MaterialTheme.colorScheme.surfaceVariant,
		contentColor = MaterialTheme.colorScheme.onSurfaceVariant,
		modifier = Modifier.fillMaxWidth(),
	) {
		Column {
			Row(
				verticalAlignment = Alignment.CenterVertically,
				horizontalArrangement = Arrangement.spacedBy(8.dp),
				modifier = Modifier
					.fillMaxWidth()
					.clickable(onClick = onOpen)
					.padding(start = 12.dp, top = 4.dp, bottom = 4.dp),
			) {
				// The same icon the uploads listing's own row uses, so the strip
				// and the way in read as one feature.
				Icon(
					Icons.Default.AddLink,
					contentDescription = null,
					modifier = Modifier.size(18.dp),
				)
				Text(
					text = label(live, moving, notice),
					style = MaterialTheme.typography.bodySmall,
					maxLines = 1,
					overflow = TextOverflow.Ellipsis,
					modifier = Modifier.weight(1f),
				)
				// Only a finished one can be dismissed. A running fetch is not the
				// user's to hide from themselves — Cancel, in the panel, is the
				// control that ends it.
				if (live.isEmpty() && notice != null) {
					IconButton(onClick = { onDismiss(notice.job.id) }) {
						Icon(
							Icons.Default.Close,
							contentDescription = "Dismiss",
							modifier = Modifier.size(18.dp),
						)
					}
				}
			}

			// Determinate only for a single job actually moving, which is the one
			// case a percentage describes. Queued work has none, and a bar frozen
			// at zero reads as a stall — the same argument the player bar makes
			// for showing an indeterminate one while buffering.
			if (moving != null && live.size == 1) {
				LinearProgressIndicator(
					progress = { moving.job.percent.coerceIn(0, 100) / 100f },
					modifier = Modifier.fillMaxWidth(),
				)
			} else if (live.isNotEmpty()) {
				LinearProgressIndicator(modifier = Modifier.fillMaxWidth())
			}
		}
	}
}

private fun label(
	live: List<FetchJobRef>,
	moving: FetchJobRef?,
	notice: FetchJobRef?,
): String {
	if (live.isEmpty()) {
		val job = notice?.job ?: return ""
		return if (FetchState.of(job.state) == FetchState.ERROR) "Fetch failed"
		else "Fetched — ${job.files} file(s)"
	}

	// Named after the one that is moving, not the first in the list: the server
	// runs one at a time, and the rest are a queue behind it.
	val head = moving ?: live.first()
	val name = with(head.job) {
		if (artist.isNotBlank() || album.isNotBlank())
			"${artist.ifBlank { "…" }} · ${album.ifBlank { "…" }}"
		else
			handler
	}
	val verb = when (FetchState.of(head.job.state)) {
		FetchState.SCANNING -> "Adding to the library"
		FetchState.QUEUED -> "Queued"
		else -> "Fetching"
	}
	val rest = live.size - 1
	return if (rest > 0) "$verb $name — and $rest more queued" else "$verb $name"
}
