package org.gaindrive.android.ui.components

import androidx.compose.foundation.background
import androidx.compose.foundation.gestures.awaitEachGesture
import androidx.compose.foundation.gestures.awaitFirstDown
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.fillMaxHeight
import androidx.compose.foundation.layout.offset
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.shape.CircleShape
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableIntStateOf
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.input.pointer.pointerInput
import androidx.compose.ui.layout.onSizeChanged
import androidx.compose.ui.platform.LocalDensity
import androidx.compose.ui.semantics.clearAndSetSemantics
import androidx.compose.ui.semantics.contentDescription
import androidx.compose.ui.unit.IntOffset
import androidx.compose.ui.unit.dp

/**
 * A vertical index of letters for jumping around a long alphabetical list.
 *
 * Sits on the **trailing** edge rather than the leading one. From Android 10 the
 * left edge is the back-gesture region, and an interactive strip there competes
 * with the system for every drag that starts near it. The trailing edge is also
 * the easier thumb reach for most people.
 *
 * Only letters the list actually contains are shown, so no tap is ever a no-op.
 * A tap and a drag are handled identically — a tap is just a press that never
 * moved — which is what makes scrubbing down the rail feel continuous.
 */
@Composable
fun AlphabetRail(
	letters: List<String>,
	onSelect: (Int) -> Unit,
	modifier: Modifier = Modifier,
) {
	if (letters.size < 2) return

	var railHeight by remember { mutableIntStateOf(0) }
	var activeIndex by remember { mutableStateOf<Int?>(null) }
	var activeY by remember { mutableIntStateOf(0) }

	Box(modifier = modifier.fillMaxHeight()) {
		Column(
			modifier = Modifier
				.fillMaxHeight()
				.width(RAIL_WIDTH)
				.align(Alignment.CenterEnd)
				.onSizeChanged { railHeight = it.height }
				.pointerInput(letters, railHeight) {
					awaitEachGesture {
						fun report(y: Float) {
							if (railHeight <= 0) return
							val fraction = (y / railHeight).coerceIn(0f, 1f)
							val index = (fraction * letters.size).toInt()
								.coerceIn(0, letters.lastIndex)
							activeY = y.toInt()
							if (index != activeIndex) {
								activeIndex = index
								onSelect(index)
							}
						}

						val down = awaitFirstDown(requireUnconsumed = false)
						report(down.position.y)
						do {
							val event = awaitPointerEvent()
							event.changes.forEach { change ->
								if (change.pressed) report(change.position.y)
								// Claimed so the list underneath does not also
								// scroll from the same drag.
								change.consume()
							}
						} while (event.changes.any { it.pressed })
						activeIndex = null
					}
				}
				// One target for screen readers; 26 single letters would be
				// noise, and the list itself is the accessible way to navigate.
				.clearAndSetSemantics { contentDescription = "Jump to letter" },
			verticalArrangement = Arrangement.SpaceEvenly,
			horizontalAlignment = Alignment.CenterHorizontally,
		) {
			letters.forEachIndexed { index, letter ->
				Text(
					text = letter,
					style = MaterialTheme.typography.labelSmall,
					color = if (index == activeIndex) {
						MaterialTheme.colorScheme.primary
					} else {
						MaterialTheme.colorScheme.onSurfaceVariant
					},
				)
			}
		}

		// Follows the finger, so the letter being selected stays readable even
		// though the rail's own labels are tiny and under the thumb.
		activeIndex?.let { index ->
			// activeY is a raw pointer coordinate in pixels, so the offsets it
			// is combined with have to be converted rather than assumed.
			val density = LocalDensity.current
			val halfBubble = with(density) { (BUBBLE_SIZE / 2).roundToPx() }
			val inset = with(density) { (RAIL_WIDTH + BUBBLE_GAP).roundToPx() }
			Box(
				modifier = Modifier
					.align(Alignment.TopEnd)
					.offset { IntOffset(x = -inset, y = activeY - halfBubble) }
					.size(BUBBLE_SIZE)
					.background(MaterialTheme.colorScheme.primary, CircleShape),
				contentAlignment = Alignment.Center,
			) {
				Text(
					text = letters[index],
					style = MaterialTheme.typography.titleLarge,
					color = MaterialTheme.colorScheme.onPrimary,
				)
			}
		}
	}
}

private val RAIL_WIDTH = 24.dp
private val BUBBLE_SIZE = 48.dp

/** Keeps the bubble clear of the thumb holding the rail. */
private val BUBBLE_GAP = 8.dp
