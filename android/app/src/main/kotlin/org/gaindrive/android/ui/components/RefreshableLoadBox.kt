package org.gaindrive.android.ui.components

import androidx.compose.material3.ExperimentalMaterial3Api
import androidx.compose.material3.pulltorefresh.PullToRefreshBox
import androidx.compose.runtime.Composable
import androidx.compose.ui.Modifier
import org.gaindrive.android.ui.Load

/**
 * [LoadStateBox] with pull-to-refresh around it.
 *
 * The two belong together but mean different things, and combining them here
 * keeps that distinction in one place: [state] going to [Load.Loading] replaces
 * the content with a spinner, whereas [isRefreshing] leaves it on screen and
 * shows only the pull indicator. A refresh that blanked the list would hide the
 * very thing the user pulled to update.
 */
@OptIn(ExperimentalMaterial3Api::class)
@Composable
fun <T> RefreshableLoadBox(
	state: Load<T>,
	isRefreshing: Boolean,
	onRefresh: () -> Unit,
	modifier: Modifier = Modifier,
	onRetry: (() -> Unit)? = null,
	content: @Composable (T) -> Unit,
) {
	PullToRefreshBox(
		isRefreshing = isRefreshing,
		onRefresh = onRefresh,
		modifier = modifier,
	) {
		LoadStateBox(state = state, onRetry = onRetry, content = content)
	}
}
