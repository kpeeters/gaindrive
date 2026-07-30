package org.gaindrive.android.ui.components

import androidx.compose.foundation.background
import androidx.compose.foundation.clickable
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.shape.CircleShape
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.filled.MusicNote
import androidx.compose.material.icons.filled.Person
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
	onClick: (() -> Unit)? = null,
) {
	val shape = RoundedCornerShape(cornerRadius)
	// Last in the chain and after the clip, so the ripple lands on top of the
	// artwork and follows its corners instead of a square.
	val clicks = if (onClick == null) Modifier else Modifier.clickable(onClick = onClick)
	if (url == null) {
		Box(
			modifier = modifier
				.clip(shape)
				.background(MaterialTheme.colorScheme.surfaceVariant)
				.then(clicks),
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
			modifier = modifier.clip(shape).then(clicks),
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

/**
 * Circular artist portrait.
 *
 * The placeholder sits *behind* the image rather than being chosen instead of
 * it: `getCoverArt` answers 404 for an artist with no portrait, and Coil then
 * draws nothing, so a placeholder picked only on a null URL would leave a blank
 * circle. Layering means a failed load falls back for free.
 */
@Composable
fun ArtistAvatar(url: String?, contentDescription: String?, size: Dp = 96.dp) {
	Box(
		modifier = Modifier
			.size(size)
			.clip(CircleShape)
			.background(MaterialTheme.colorScheme.surfaceVariant),
		contentAlignment = Alignment.Center,
	) {
		Icon(
			imageVector = Icons.Default.Person,
			contentDescription = null,
			tint = MaterialTheme.colorScheme.onSurfaceVariant,
			modifier = Modifier.size(size / 2),
		)
		if (url != null) {
			AsyncImage(
				model = url,
				contentDescription = contentDescription,
				contentScale = ContentScale.Crop,
				modifier = Modifier.fillMaxSize(),
			)
		}
	}
}

/** Fills its parent, for the album detail hero and the now-playing sheet. */
@Composable
fun CoverHero(
	url: String?,
	contentDescription: String?,
	modifier: Modifier = Modifier,
	onClick: (() -> Unit)? = null,
) {
	CoverArt(
		url = url,
		contentDescription = contentDescription,
		modifier = modifier.fillMaxSize(),
		cornerRadius = 8.dp,
		onClick = onClick,
	)
}
