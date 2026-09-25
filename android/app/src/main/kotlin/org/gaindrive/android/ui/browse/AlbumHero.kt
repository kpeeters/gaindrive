package org.gaindrive.android.ui.browse

import androidx.compose.foundation.background
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.aspectRatio
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.pager.HorizontalPager
import androidx.compose.foundation.pager.rememberPagerState
import androidx.compose.foundation.shape.CircleShape
import androidx.compose.material3.MaterialTheme
import androidx.compose.runtime.Composable
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clip
import androidx.compose.ui.unit.dp
import org.gaindrive.android.ui.LocalIsTv
import org.gaindrive.android.ui.components.CoverHero

/**
 * The album's cover, paging through the folder's extra images when it has
 * any, as the iOS album screen does.
 *
 * A TV gets the cover alone: the hero is not a focus stop there, and making it
 * one would put a d-pad detour between the top bar and the first track.
 */
@Composable
fun AlbumHero(
	urls: List<String>,
	contentDescription: String,
	modifier: Modifier = Modifier,
) {
	if (urls.size <= 1 || LocalIsTv.current) {
		CoverHero(
			url = urls.firstOrNull(),
			contentDescription = contentDescription,
			modifier = modifier.aspectRatio(1f).padding(16.dp),
		)
		return
	}

	val pagerState = rememberPagerState(pageCount = { urls.size })
	Column(modifier = modifier, horizontalAlignment = Alignment.CenterHorizontally) {
		HorizontalPager(
			state = pagerState,
			// Keyed on the URL so a page that arrives with the image count does
			// not reload the cover already on screen.
			key = { urls[it] },
			modifier = Modifier.fillMaxWidth().aspectRatio(1f),
		) { page ->
			CoverHero(
				url = urls[page],
				// Only the cover is described; the rest are pictures of the same
				// record and a count says more than a repeated title.
				contentDescription =
					if (page == 0) contentDescription
					else "Image ${page + 1} of ${urls.size}",
				modifier = Modifier.padding(16.dp),
			)
		}
		PageDots(count = urls.size, current = pagerState.currentPage)
	}
}

@Composable
private fun PageDots(count: Int, current: Int) {
	Row(
		horizontalArrangement = Arrangement.spacedBy(6.dp),
		modifier = Modifier.padding(bottom = 8.dp),
	) {
		repeat(count) { index ->
			Box(
				modifier = Modifier
					.size(6.dp)
					.clip(CircleShape)
					.background(
						if (index == current) MaterialTheme.colorScheme.primary
						else MaterialTheme.colorScheme.outlineVariant
					)
			)
		}
	}
}
