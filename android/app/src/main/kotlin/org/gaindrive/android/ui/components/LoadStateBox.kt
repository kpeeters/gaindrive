package org.gaindrive.android.ui.components

import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.padding
import androidx.compose.material3.CircularProgressIndicator
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.OutlinedButton
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.text.style.TextAlign
import androidx.compose.ui.unit.dp
import org.gaindrive.android.ui.Load

/**
 * Renders the loading and failure arms of a [Load] so no screen has to repeat
 * them, and so they look the same everywhere.
 */
@Composable
fun <T> LoadStateBox(
	state: Load<T>,
	onRetry: (() -> Unit)? = null,
	modifier: Modifier = Modifier,
	content: @Composable (T) -> Unit,
) {
	when (state) {
		is Load.Loading -> Box(
			modifier = modifier.fillMaxSize(),
			contentAlignment = Alignment.Center,
		) {
			CircularProgressIndicator()
		}

		is Load.Failed -> Box(
			modifier = modifier.fillMaxSize().padding(24.dp),
			contentAlignment = Alignment.Center,
		) {
			Column(
				horizontalAlignment = Alignment.CenterHorizontally,
				verticalArrangement = Arrangement.spacedBy(12.dp),
			) {
				Text(
					text = state.message,
					style = MaterialTheme.typography.bodyMedium,
					color = MaterialTheme.colorScheme.onSurfaceVariant,
					textAlign = TextAlign.Center,
				)
				if (onRetry != null) {
					OutlinedButton(onClick = onRetry) { Text("Try again") }
				}
			}
		}

		is Load.Ready -> content(state.value)
	}
}

/** For a list that loaded fine and simply has nothing in it. */
@Composable
fun EmptyMessage(text: String, modifier: Modifier = Modifier) {
	Box(
		modifier = modifier.fillMaxSize().padding(24.dp),
		contentAlignment = Alignment.Center,
	) {
		Text(
			text = text,
			style = MaterialTheme.typography.bodyMedium,
			color = MaterialTheme.colorScheme.onSurfaceVariant,
			textAlign = TextAlign.Center,
		)
	}
}
