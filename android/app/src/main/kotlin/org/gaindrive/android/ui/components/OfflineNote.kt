package org.gaindrive.android.ui.components

import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.padding
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Surface
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.ui.Modifier
import androidx.compose.ui.text.style.TextAlign
import androidx.compose.ui.unit.dp

/**
 * Says the app is not talking to any server, and what that means here.
 *
 * Not dismissible: it explains every dimmed row on screen for as long as it
 * lasts, and unlike a server that did not answer there is nothing to retry.
 *
 * [byChoice] changes the wording rather than the presence, because the two
 * cases call for different reactions: one is something to fix, the other is
 * something the user switched on and may have forgotten.
 */
@Composable
fun OfflineNote(online: Boolean, byChoice: Boolean) {
	if (online) return

	Surface(
		color = MaterialTheme.colorScheme.surfaceVariant,
		contentColor = MaterialTheme.colorScheme.onSurfaceVariant,
		modifier = Modifier.fillMaxWidth(),
	) {
		Text(
			text = if (byChoice) "Offline mode — only stored music can play"
			else "No network — only stored music can play",
			style = MaterialTheme.typography.bodySmall,
			textAlign = TextAlign.Center,
			modifier = Modifier.fillMaxWidth().padding(vertical = 4.dp),
		)
	}
}
