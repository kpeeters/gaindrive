package org.gaindrive.android.ui.components

import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.padding
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.filled.Close
import androidx.compose.material3.Icon
import androidx.compose.material3.IconButton
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Surface
import androidx.compose.material3.Text
import androidx.compose.material3.TextButton
import androidx.compose.runtime.Composable
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.unit.dp
import org.gaindrive.android.data.ServerFailure

/**
 * Names the servers that did not answer, above content built from the ones
 * that did.
 *
 * Inline and dismissible rather than a dialog or an error screen: with several
 * servers configured, one being unreachable is a degraded view, not a failed
 * one, and a screen must never be blank because the least important of three
 * servers is down.
 */
@Composable
fun PartialFailureNote(
	failures: List<ServerFailure>,
	onRetry: () -> Unit,
	onDismiss: () -> Unit,
	modifier: Modifier = Modifier,
) {
	if (failures.isEmpty()) return

	Surface(
		color = MaterialTheme.colorScheme.errorContainer,
		contentColor = MaterialTheme.colorScheme.onErrorContainer,
		modifier = modifier.fillMaxWidth(),
	) {
		Row(
			modifier = Modifier.padding(start = 16.dp, top = 8.dp, bottom = 8.dp),
			verticalAlignment = Alignment.CenterVertically,
		) {
			Column(
				modifier = Modifier.weight(1f),
				verticalArrangement = Arrangement.spacedBy(2.dp),
			) {
				failures.forEach { failure ->
					// Named one per line: "two servers failed" tells the user
					// nothing about which of their libraries is missing.
					Text(
						text = "${failure.serverName}: ${failure.message}",
						style = MaterialTheme.typography.bodySmall,
					)
				}
			}
			TextButton(onClick = onRetry) { Text("Retry") }
			IconButton(onClick = onDismiss) {
				Icon(Icons.Default.Close, contentDescription = "Dismiss")
			}
		}
	}
}
