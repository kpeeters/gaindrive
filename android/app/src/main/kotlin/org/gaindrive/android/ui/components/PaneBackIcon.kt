package org.gaindrive.android.ui.components

import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.automirrored.filled.ArrowBack
import androidx.compose.material3.Icon
import androidx.compose.material3.IconButton
import androidx.compose.runtime.Composable

/**
 * The navigation icon of a pane's own app bar.
 *
 * Null draws nothing, which is the ordinary case on a tablet: with the level
 * above already on screen in the pane beside this one, an arrow pointing at it
 * is noise. Every screen that can be a non-root pane takes its `onBack` as a
 * nullable and hands it here, so the rule lives in one place —
 * `LocalPaneBack`, which decides it from the layout.
 */
@Composable
fun PaneBackIcon(onBack: (() -> Unit)?) {
	if (onBack == null) return
	IconButton(onClick = onBack) {
		Icon(Icons.AutoMirrored.Filled.ArrowBack, contentDescription = "Back")
	}
}
