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
 * Says the device has no network, and what that means here.
 *
 * Not dismissible: unlike a server that did not answer, this is not a condition
 * the user can retry past, and it explains every dimmed row on screen for as
 * long as it lasts.
 */
@Composable
fun OfflineNote(online: Boolean) {
	if (online) return

	Surface(
		color = MaterialTheme.colorScheme.surfaceVariant,
		contentColor = MaterialTheme.colorScheme.onSurfaceVariant,
		modifier = Modifier.fillMaxWidth(),
	) {
		Text(
			text = "Offline — only downloaded music can play",
			style = MaterialTheme.typography.bodySmall,
			textAlign = TextAlign.Center,
			modifier = Modifier.fillMaxWidth().padding(vertical = 4.dp),
		)
	}
}
