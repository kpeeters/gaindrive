package org.gaindrive.android.ui.components

import androidx.compose.foundation.background
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.filled.MusicNote
import androidx.compose.material3.Icon
import androidx.compose.material3.MaterialTheme
import androidx.compose.runtime.Composable
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clip
import androidx.compose.ui.layout.ContentScale
import androidx.compose.ui.unit.Dp
import androidx.compose.ui.unit.dp
import coil3.compose.AsyncImage

/**
 * Album or artist artwork with a placeholder for the common case of an album
 * that has none.
 *
 * The URL is built by [org.gaindrive.android.data.CoverUrls] and already
 * carries the server's auth parameters, so Coil needs no interceptor of its
 * own.
 */
@Composable
fun CoverArt(
	url: String?,
	contentDescription: String?,
	modifier: Modifier = Modifier,
	cornerRadius: Dp = 4.dp,
) {
	val shape = RoundedCornerShape(cornerRadius)
	if (url == null) {
		Box(
			modifier = modifier
				.clip(shape)
				.background(MaterialTheme.colorScheme.surfaceVariant),
			contentAlignment = Alignment.Center,
		) {
			Icon(
				imageVector = Icons.Default.MusicNote,
				contentDescription = contentDescription,
				tint = MaterialTheme.colorScheme.onSurfaceVariant,
				modifier = Modifier.size(20.dp),
			)
		}
	} else {
		AsyncImage(
			model = url,
			contentDescription = contentDescription,
			contentScale = ContentScale.Crop,
			modifier = modifier.clip(shape),
		)
	}
}

/** List-thumbnail sized cover; the size matches what the server is asked for. */
@Composable
fun CoverThumb(url: String?, contentDescription: String?, size: Dp = 48.dp) {
	CoverArt(
		url = url,
		contentDescription = contentDescription,
		modifier = Modifier.size(size),
	)
}

/** Fills its parent, for the album detail hero. */
@Composable
fun CoverHero(url: String?, contentDescription: String?, modifier: Modifier = Modifier) {
	CoverArt(
		url = url,
		contentDescription = contentDescription,
		modifier = modifier.fillMaxSize(),
		cornerRadius = 8.dp,
	)
}
