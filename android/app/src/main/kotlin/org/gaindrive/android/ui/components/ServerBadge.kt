package org.gaindrive.android.ui.components

import androidx.compose.foundation.background
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clip
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.dp

/**
 * Which server a row came from.
 *
 * Deliberately quiet — a tinted label rather than a chip. It appears on every
 * row in merged scope, so anything louder would compete with the content it is
 * annotating. Callers pass null in single-server scope, and nothing renders.
 */
@Composable
fun ServerBadge(name: String?) {
	if (name == null) return
	Text(
		text = name,
		style = MaterialTheme.typography.labelSmall,
		color = MaterialTheme.colorScheme.onSurfaceVariant,
		maxLines = 1,
		overflow = TextOverflow.Ellipsis,
		modifier = Modifier
			.clip(RoundedCornerShape(4.dp))
			.background(MaterialTheme.colorScheme.surfaceVariant)
			.padding(horizontal = 6.dp, vertical = 2.dp),
	)
}

/** Several of them, for a row that stands for the same artist on each. */
@Composable
fun ServerBadges(names: List<String>) {
	if (names.size < 2) {
		ServerBadge(names.firstOrNull())
		return
	}
	Row(horizontalArrangement = Arrangement.spacedBy(4.dp)) {
		names.forEach { ServerBadge(it) }
	}
}
