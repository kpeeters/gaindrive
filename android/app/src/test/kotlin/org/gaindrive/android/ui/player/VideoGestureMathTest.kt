package org.gaindrive.android.ui.player

import org.junit.Assert.assertEquals
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Test

/**
 * Which side does what, and which way is up.
 *
 * Both are wrong silently rather than loudly — a swapped pair of zones is a
 * player where the volume dims the screen, an inverted sign is one where
 * swiping up turns the sound down, and neither crashes, logs or fails to
 * compile. They are also the only part of the gesture that can be reached from
 * a JVM test: everything around them is a pointer stream, a window attribute
 * and an `AudioManager`.
 *
 * A 1000-pixel width with a 50-pixel inset puts the boundaries at 50, 400, 600
 * and 950 for the 0.4 zone fraction the file holds.
 */
class VideoGestureMathTest {

	@Test
	fun `the left of the picture is brightness and the right is volume`() {
		assertEquals(SideControl.Brightness, sideControlAt(100f, 1000f, 50f))
		assertEquals(SideControl.Volume, sideControlAt(900f, 1000f, 50f))
	}

	/** A stray vertical drag across the film must do nothing at all. */
	@Test
	fun `the middle starts nothing`() {
		assertNull(sideControlAt(500f, 1000f, 50f))
		assertNull(sideControlAt(410f, 1000f, 50f))
		assertNull(sideControlAt(590f, 1000f, 50f))
	}

	/**
	 * The outer margin belongs to the system's back gesture, on both edges —
	 * the trailing one carries it too from Android 10.
	 */
	@Test
	fun `neither edge is a zone`() {
		assertNull(sideControlAt(10f, 1000f, 50f))
		assertNull(sideControlAt(990f, 1000f, 50f))
	}

	/** Nothing has been laid out yet, and a ratio against zero is not a number. */
	@Test
	fun `a zero width starts nothing`() {
		assertNull(sideControlAt(0f, 0f, 50f))
		assertEquals(0f, travelFraction(-100f, 0f), 0f)
	}

	/**
	 * Pointer y grows downwards; every control of this shape goes the other
	 * way. This is the assertion that catches the sign being tidied away.
	 */
	@Test
	fun `dragging up increases and dragging down decreases`() {
		assertTrue(travelFraction(-100f, 1000f) > 0f)
		assertTrue(travelFraction(100f, 1000f) < 0f)
		assertEquals(0f, travelFraction(0f, 1000f), 1e-4f)
	}

	/** A sweep shorter than the screen has to cover the whole range. */
	@Test
	fun `one screen height is more than the whole range`() {
		assertTrue(travelFraction(-1000f, 1000f) >= 1f)
	}
}
