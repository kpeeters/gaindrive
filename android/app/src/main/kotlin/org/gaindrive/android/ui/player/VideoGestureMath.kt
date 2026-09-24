package org.gaindrive.android.ui.player

/** Which of the two things a vertical swipe down the side of a film drives. */
enum class SideControl { Brightness, Volume }

/**
 * The control a touch at [x] starts, or null where a touch starts nothing.
 *
 * Pure arithmetic over pixels, and separate from the gesture that calls it
 * because the two things most easily got wrong here - which side is which, and
 * where the zones stop - are silent when wrong and cheap to test.
 *
 * Two parts of the width are deliberately not zones. The outer [edgeInset] is
 * left to the system: from Android 10 the screen edges are the back-gesture
 * region, and anything interactive there competes for every drag beginning
 * near it, which is the same reason `AlphabetRail` sits on the trailing edge
 * rather than the leading one. And the middle is dead, so a stray vertical
 * drag across the picture does nothing at all rather than something nobody
 * asked for.
 */
fun sideControlAt(x: Float, width: Float, edgeInset: Float): SideControl? {
	if (width <= 0f) return null
	return when {
		x < edgeInset || x > width - edgeInset -> null
		x < width * ZONE_FRACTION -> SideControl.Brightness
		x > width * (1f - ZONE_FRACTION) -> SideControl.Volume
		else -> null
	}
}

/**
 * How much of a control's range [dy] pixels of finger travel is worth.
 *
 * **Up increases**, hence the inverted sign: pointer y grows downwards and
 * every physical control of this shape goes the other way.
 *
 * [FULL_TRAVEL] is short of the whole height so the full range is reachable
 * without a swipe that has to begin or end at an edge, where the system is
 * waiting for a gesture of its own.
 */
fun travelFraction(dy: Float, height: Float): Float {
	if (height <= 0f) return 0f
	return -dy / (height * FULL_TRAVEL)
}

/** How much of the width each side claims, leaving a dead middle fifth. */
private const val ZONE_FRACTION = 0.4f

/** The share of the height one end-to-end sweep covers. */
private const val FULL_TRAVEL = 0.7f
