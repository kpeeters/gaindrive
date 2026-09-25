package org.gaindrive.android.ui.components

import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.filled.Star
import androidx.compose.material.icons.filled.StarBorder
import androidx.compose.material3.Icon
import androidx.compose.material3.IconButton
import androidx.compose.material3.LocalContentColor
import androidx.compose.material3.MaterialTheme
import androidx.compose.runtime.Composable
import androidx.compose.ui.Modifier
import org.gaindrive.android.data.model.ItemRef
import org.gaindrive.android.data.model.StarKind
import org.gaindrive.android.ui.LocalStars

/**
 * The visible star toggle, for a header or the player. [fallback] is what the
 * caller's model says; [noun] names the thing for TalkBack ("album", "track").
 */
@Composable
fun StarButton(
	ref: ItemRef,
	kind: StarKind,
	fallback: Boolean,
	noun: String,
	modifier: Modifier = Modifier,
	enabled: Boolean = true,
) {
	val stars = LocalStars.current
	val starred = stars.isStarred(ref, fallback)
	IconButton(
		onClick = { stars.toggle(ref, kind, fallback) },
		// Disabled while a write is in flight: see StarStore.busy.
		enabled = enabled && !stars.isBusy(ref),
		modifier = modifier,
	) {
		Icon(
			imageVector = if (starred) Icons.Default.Star else Icons.Default.StarBorder,
			contentDescription = if (starred) "Unstar $noun" else "Star $noun",
			// Accent when set, as the web client's star is. Otherwise whatever
			// the container hands out, which is how the outline dims itself
			// while disabled.
			tint =
				if (starred) MaterialTheme.colorScheme.primary
				else LocalContentColor.current,
		)
	}
}
