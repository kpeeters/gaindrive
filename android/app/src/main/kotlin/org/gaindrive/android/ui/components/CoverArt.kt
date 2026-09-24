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
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableIntStateOf
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clip
import androidx.compose.ui.layout.ContentScale
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.unit.Dp
import androidx.compose.ui.unit.dp
import coil3.compose.AsyncImage
import coil3.request.ImageRequest
import kotlinx.coroutines.delay
import org.gaindrive.android.data.cache.artCaching
import org.gaindrive.android.ui.LocalAvailability

/**
 * Album or artist artwork with a placeholder for the common case of an album
 * that has none.
 *
 * The URL is built by [org.gaindrive.android.data.CoverUrls] and already
 * carries the server's auth parameters, so Coil needs no interceptor of its
 * own. It is not, however, a usable cache key (see
 * [org.gaindrive.android.data.cache.ArtKeys]), which is what `artCaching`
 * corrects here.
 *
 * The placeholder sits *behind* the image rather than replacing it, for the
 * same reason [ArtistAvatar] layers its own: a load that fails draws nothing,
 * and offline with nothing stored is a load that fails. Choosing the icon only
 * on a null URL would leave an empty box in exactly the case it exists for.
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
	val online = LocalAvailability.current.online
	val context = LocalContext.current
	val request = remember(url, online) {
		url?.let {
			ImageRequest.Builder(context).data(it).artCaching(it, online).build()
		}
	}
	Box(
		modifier = modifier
			.clip(shape)
			.background(MaterialTheme.colorScheme.surfaceVariant)
			.then(clicks),
		contentAlignment = Alignment.Center,
	) {
		Icon(
			imageVector = Icons.Default.MusicNote,
			// Carried by the image when there is one, so a screen reader is
			// not told the same thing twice.
			contentDescription = if (request == null) contentDescription else null,
			tint = MaterialTheme.colorScheme.onSurfaceVariant,
			modifier = Modifier.size(20.dp),
		)
		if (request != null) {
			AsyncImage(
				model = request,
				contentDescription = contentDescription,
				contentScale = ContentScale.Crop,
				modifier = Modifier.fillMaxSize(),
			)
		}
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
 * Circular artist portrait, which asks again when the server says "not yet".
 *
 * The placeholder sits *behind* the image rather than being chosen instead of
 * it: `getCoverArt` answers 404 for an artist with no portrait, and Coil then
 * draws nothing, so a placeholder picked only on a null URL would leave a blank
 * circle. Layering means a failed load falls back for free.
 *
 * **A portrait 404 does not mean there is no portrait.** Unlike an album cover,
 * which is a file the server already has, an artist portrait has to be found -
 * MusicBrainz, then Wikidata, then Wikipedia, then a couple of others - and the
 * server does that on a background thread. The first request only pushes that
 * artist to the front of the queue and answers 404; the picture exists seconds
 * later. Without a retry nothing ever asks again, so the portrait appears only
 * if the user happens to leave the screen and come back, and pull-to-refresh
 * cannot fix it either: the URL it recomputes is identical, so the state never
 * changes and Coil is never asked a second time.
 *
 * The attempt counter therefore has to be *in the URL*, and it stays there
 * even though [org.gaindrive.android.data.cache.ArtKeys] strips it back out of
 * the cache key. That split is the point: the URL has to differ or nothing is
 * re-requested, while the key has to not differ or a portrait that finally
 * arrives on attempt three is filed under a string no later screen will ever
 * ask for. Coil keys its caches on the model, so re-issuing the same string
 * would be a no-op, and the shared OkHttp cache may be holding a cacheable 404
 * for an artist the server has since resolved. A server that means "there is
 * genuinely none" says so with a `max-age`, which this will re-ask a handful
 * of times and then leave alone.
 */
@Composable
fun ArtistAvatar(url: String?, contentDescription: String?, size: Dp = 96.dp) {
	// Both reset when the URL does, i.e. when this avatar is a different artist.
	var attempt by remember(url) { mutableIntStateOf(0) }
	var failed by remember(url) { mutableStateOf(false) }

	val online = LocalAvailability.current.online
	val context = LocalContext.current
	val model = when {
		url == null -> null
		attempt == 0 -> url
		else -> "$url&_r=$attempt"
	}
	val request = remember(model, online) {
		model?.let {
			ImageRequest.Builder(context).data(it).artCaching(it, online).build()
		}
	}

	LaunchedEffect(url, failed, attempt, online) {
		// Offline the server cannot go and find a portrait, so every attempt
		// is a guaranteed failure and the ladder is pure battery.
		if (!online) return@LaunchedEffect
		if (url == null || !failed || attempt >= PORTRAIT_RETRIES) return@LaunchedEffect
		// Backing off: an artist resolved straight from the front of the queue
		// takes a few seconds, one queued behind a first-run backlog takes
		// longer, and there is no point asking at a fixed fast rate for either.
		delay(PORTRAIT_RETRY_STEP_MS * (attempt + 1))
		failed = false
		attempt++
	}

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
		if (request != null) {
			AsyncImage(
				model = request,
				contentDescription = contentDescription,
				contentScale = ContentScale.Crop,
				modifier = Modifier.fillMaxSize(),
				onError = { failed = true },
				onSuccess = { failed = false },
			)
		}
	}
}

/** Retries of a 404'd artist portrait, spread over about a minute. */
private const val PORTRAIT_RETRIES = 5
private const val PORTRAIT_RETRY_STEP_MS = 4_000L

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
