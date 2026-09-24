package org.gaindrive.android.ui.components

import androidx.compose.animation.animateContentSize
import androidx.compose.foundation.clickable
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Column
import androidx.compose.material3.AssistChip
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.Modifier
import androidx.compose.ui.platform.LocalUriHandler
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.dp

/** An outbound link shown as a chip. */
data class ExternalLink(val label: String, val url: String)

/**
 * Prose with outbound links, clamped to a few lines and expandable - the same
 * treatment the web client gives album notes and artist biographies.
 *
 * Shared between the artist header and album detail so the two cannot drift
 * apart in behaviour or spacing.
 */
@Composable
fun NotesSection(
	text: String?,
	links: List<ExternalLink>,
	modifier: Modifier = Modifier,
	collapsedLines: Int = 4,
) {
	if (text.isNullOrBlank() && links.isEmpty()) return

	var expanded by remember { mutableStateOf(false) }
	val uriHandler = LocalUriHandler.current

	Column(
		modifier = modifier,
		verticalArrangement = Arrangement.spacedBy(8.dp),
	) {
		if (!text.isNullOrBlank()) {
			Text(
				text = text,
				style = MaterialTheme.typography.bodyMedium,
				color = MaterialTheme.colorScheme.onSurfaceVariant,
				maxLines = if (expanded) Int.MAX_VALUE else collapsedLines,
				overflow = TextOverflow.Ellipsis,
				modifier = Modifier
					.animateContentSize()
					.clickable { expanded = !expanded },
			)
			Text(
				text = if (expanded) "less" else "more",
				style = MaterialTheme.typography.labelMedium,
				color = MaterialTheme.colorScheme.primary,
				modifier = Modifier.clickable { expanded = !expanded },
			)
		}

		if (links.isNotEmpty()) {
			// Scrolls rather than wraps: four links do not fit across a phone,
			// and a plain Row kept squeezing the last chip until its label
			// broke mid-word.
			ChipRow {
				links.forEach { link ->
					AssistChip(
						onClick = { uriHandler.openUri(link.url) },
						label = {
							// A chip label is a proper noun - breaking it across
							// two lines is never the right answer, so if one is
							// ever narrow enough to need it, ellipsise instead.
							Text(link.label, maxLines = 1, overflow = TextOverflow.Ellipsis)
						},
					)
				}
			}
		}
	}
}
