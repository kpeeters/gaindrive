package org.gaindrive.android.ui.browse

import androidx.compose.foundation.ExperimentalFoundationApi
import androidx.compose.foundation.background
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.lazy.items
import androidx.compose.material3.ExperimentalMaterial3Api
import androidx.compose.material3.HorizontalDivider
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Scaffold
import androidx.compose.material3.Text
import androidx.compose.material3.TopAppBar
import androidx.compose.runtime.Composable
import androidx.compose.runtime.getValue
import androidx.compose.ui.Modifier
import androidx.compose.ui.unit.dp
import androidx.hilt.navigation.compose.hiltViewModel
import androidx.lifecycle.compose.collectAsStateWithLifecycle
import org.gaindrive.android.data.model.ItemRef
import org.gaindrive.android.ui.components.ArtistRow
import org.gaindrive.android.ui.components.EmptyMessage
import org.gaindrive.android.ui.components.LoadStateBox

@OptIn(ExperimentalMaterial3Api::class, ExperimentalFoundationApi::class)
@Composable
fun ArtistsScreen(
	onOpenArtist: (ItemRef, String) -> Unit,
	viewModel: ArtistsViewModel = hiltViewModel(),
) {
	val state by viewModel.state.collectAsStateWithLifecycle()

	Scaffold(topBar = { TopAppBar(title = { Text("Artists") }) }) { insets ->
		LoadStateBox(
			state = state,
			onRetry = viewModel::load,
			modifier = Modifier.padding(insets),
		) { indexes ->
			if (indexes.isEmpty()) {
				EmptyMessage("This library has no artists yet.")
				return@LoadStateBox
			}

			LazyColumn(modifier = Modifier.fillMaxSize()) {
				indexes.forEach { index ->
					stickyHeader(key = "hdr-${index.label}") {
						Text(
							text = index.label,
							style = MaterialTheme.typography.labelLarge,
							color = MaterialTheme.colorScheme.primary,
							modifier = Modifier
								.fillMaxWidth()
								.background(MaterialTheme.colorScheme.background)
								.padding(horizontal = 16.dp, vertical = 6.dp),
						)
					}
					items(index.artists, key = { it.ref.encode() }) { artist ->
						ArtistRow(artist) { onOpenArtist(artist.ref, artist.name) }
						HorizontalDivider(
							color = MaterialTheme.colorScheme.outlineVariant,
						)
					}
				}
			}
		}
	}
}
