package org.gaindrive.android.ui.components

import androidx.compose.foundation.horizontalScroll
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.RowScope
import androidx.compose.foundation.rememberScrollState
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.ui.Modifier
import androidx.compose.ui.unit.dp

/**
 * A single line of chips that scrolls sideways when there are too many to fit.
 *
 * Wrapping was the other option and reads worse: a second line of chips looks
 * like a second group of options, and the row's height changes with the device
 * width, so everything below it moves. Scrolling keeps the row one line high
 * whatever is in it, and a chip cut off at the edge is its own invitation to
 * swipe.
 *
 * [selectedIndex] and [chipCount], when given, bring the selected chip into
 * view after the row is first measured — otherwise a setting near the end of a
 * long row is invisible until the user thinks to swipe, which makes it look
 * unset. Only on first layout: re-running it on every tap would yank the row
 * sideways under the finger.
 */
@Composable
fun ChipRow(
	modifier: Modifier = Modifier,
	selectedIndex: Int = -1,
	chipCount: Int = 0,
	content: @Composable RowScope.() -> Unit,
) {
	val scroll = rememberScrollState()

	// Keyed on maxValue alone, which is 0 until the row has been laid out and
	// settles once: that makes this fire exactly when the measurement it needs
	// becomes available, and not again when the selection changes.
	LaunchedEffect(scroll.maxValue) {
		if (scroll.maxValue == 0 || selectedIndex < 0 || chipCount < 2) return@LaunchedEffect
		// Proportional rather than measured. Chips differ in width, so this is
		// an approximation — but it is monotonic and lands exactly on 0 and on
		// maxValue at the ends, which are the two cases that matter.
		val fraction = selectedIndex.toFloat() / (chipCount - 1)
		scroll.scrollTo((scroll.maxValue * fraction).toInt())
	}

	Row(
		modifier = modifier.horizontalScroll(scroll),
		horizontalArrangement = Arrangement.spacedBy(8.dp),
		content = content,
	)
}
