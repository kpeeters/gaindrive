package org.gaindrive.android.ui.settings

import androidx.compose.foundation.clickable
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.PaddingValues
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.lazy.LazyListScope
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.automirrored.filled.ArrowBack
import androidx.compose.material.icons.automirrored.filled.KeyboardArrowRight
import androidx.compose.material3.ExperimentalMaterial3Api
import androidx.compose.material3.Icon
import androidx.compose.material3.IconButton
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Scaffold
import androidx.compose.material3.Switch
import androidx.compose.material3.Text
import androidx.compose.material3.TopAppBar
import androidx.compose.runtime.Composable
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.dp

/** Pieces every settings screen is built from. */

/**
 * The frame each settings screen shares.
 *
 * [onBack] is null on the top-level screen, which is a tab and has nowhere to
 * go back to.
 */
@OptIn(ExperimentalMaterial3Api::class)
@Composable
fun SettingsScaffold(
	title: String,
	onBack: (() -> Unit)?,
	floatingActionButton: @Composable () -> Unit = {},
	content: LazyListScope.() -> Unit,
) {
	Scaffold(
		topBar = {
			TopAppBar(
				title = { Text(title) },
				navigationIcon = {
					onBack?.let { back ->
						IconButton(onClick = back) {
							Icon(
								Icons.AutoMirrored.Filled.ArrowBack,
								contentDescription = "Back",
							)
						}
					}
				},
			)
		},
		floatingActionButton = floatingActionButton,
	) { insets ->
		LazyColumn(
			modifier = Modifier.fillMaxSize().padding(insets),
			contentPadding = PaddingValues(16.dp),
			verticalArrangement = Arrangement.spacedBy(12.dp),
			content = content,
		)
	}
}

/**
 * A row on the top-level screen that opens a sub-screen.
 *
 * [summary] is the current state, not a description of the category. It is what
 * makes the top level worth reading rather than a list of words you already
 * know — most visits are to check a value, not to change one.
 */
@Composable
fun CategoryRow(title: String, summary: String, onClick: () -> Unit) {
	Row(
		modifier = Modifier
			.fillMaxWidth()
			.clickable(onClick = onClick)
			.padding(vertical = 8.dp),
		verticalAlignment = Alignment.CenterVertically,
		horizontalArrangement = Arrangement.spacedBy(12.dp),
	) {
		Column(modifier = Modifier.weight(1f)) {
			Text(text = title, style = MaterialTheme.typography.bodyLarge)
			Text(
				text = summary,
				style = MaterialTheme.typography.bodySmall,
				color = MaterialTheme.colorScheme.onSurfaceVariant,
				maxLines = 2,
				overflow = TextOverflow.Ellipsis,
			)
		}
		Icon(
			Icons.AutoMirrored.Filled.KeyboardArrowRight,
			contentDescription = null,
			tint = MaterialTheme.colorScheme.onSurfaceVariant,
		)
	}
}

@Composable
fun SwitchRow(
	title: String,
	subtitle: String,
	checked: Boolean,
	onCheckedChange: (Boolean) -> Unit,
) {
	Row(
		modifier = Modifier.fillMaxWidth(),
		verticalAlignment = Alignment.CenterVertically,
		horizontalArrangement = Arrangement.spacedBy(12.dp),
	) {
		Column(modifier = Modifier.weight(1f)) {
			Text(text = title, style = MaterialTheme.typography.bodyLarge)
			Text(
				text = subtitle,
				style = MaterialTheme.typography.bodySmall,
				color = MaterialTheme.colorScheme.onSurfaceVariant,
			)
		}
		Switch(checked = checked, onCheckedChange = onCheckedChange)
	}
}

@Composable
fun SectionTitle(text: String) {
	Text(
		text = text,
		style = MaterialTheme.typography.titleMedium,
		color = MaterialTheme.colorScheme.primary,
		modifier = Modifier.padding(top = 8.dp),
	)
}
