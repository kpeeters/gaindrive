package org.gaindrive.android.ui.adaptive

import org.junit.Assert.assertEquals
import org.junit.Assert.assertTrue
import org.junit.Test

/**
 * The pane strip's two window rules.
 *
 * Worth testing at all because the failure they guard against is silent and
 * total: a mapping that leaves the level being read off screen is a blank pane
 * with no error anywhere, and it would only appear at one particular width.
 * `everyWidthShowsTheCurrentLevel` is the whole point of the file; the rest
 * pins the cases that describe the design.
 */
class PaneWindowTest {

	@Test
	fun `one pane shows only the level being read`() {
		assertEquals(
			PaneSlots(Pane.Gone, Pane.Gone, Pane.At(2)),
			leadingWindow(depth = 2, panes = 1, levels = 3),
		)
	}

	@Test
	fun `the level being read is the trailing visible pane`() {
		// Artists is off the leading edge; albums and tracks are what is left.
		assertEquals(
			PaneSlots(Pane.Gone, Pane.At(1), Pane.At(2)),
			leadingWindow(depth = 2, panes = 2, levels = 3),
		)
	}

	@Test
	fun `three panes show the whole library path`() {
		assertEquals(
			PaneSlots(Pane.At(0), Pane.At(1), Pane.At(2)),
			leadingWindow(depth = 2, panes = 3, levels = 3),
		)
	}

	@Test
	fun `a level with nothing chosen yet waits rather than disappearing`() {
		assertEquals(
			PaneSlots(Pane.At(0), Pane.Waiting, Pane.Gone),
			leadingWindow(depth = 0, panes = 2, levels = 3),
		)
	}

	@Test
	fun `only one level ever waits`() {
		// Three panes with nothing chosen. The third would otherwise ask for
		// an album while no artist is picked - a choice that cannot be made -
		// so it is blank: on screen for its width, saying nothing.
		assertEquals(
			PaneSlots(Pane.At(0), Pane.Waiting, Pane.Blank),
			leadingWindow(depth = 0, panes = 3, levels = 3),
		)
		// One below the level being read still waits, which is the case the
		// bound above must not break.
		assertEquals(
			PaneSlots(Pane.At(0), Pane.At(1), Pane.Waiting),
			leadingWindow(depth = 1, panes = 3, levels = 3),
		)
	}

	@Test
	fun `a two-level tab never draws a third pane`() {
		// Playlists. Without the `levels` bound a wide window would offer a
		// choice at a level that does not exist.
		assertEquals(
			PaneSlots(Pane.At(0), Pane.At(1), Pane.Gone),
			leadingWindow(depth = 1, panes = 3, levels = 2),
		)
	}

	@Test
	fun `search drops the middle level, not the results`() {
		// The one place the two rules disagree, and the reason searchWindow
		// exists: leadingWindow would hide the results here.
		assertEquals(
			PaneSlots(Pane.Gone, Pane.At(1), Pane.At(2)),
			leadingWindow(depth = 2, panes = 2, levels = 3),
		)
		assertEquals(
			PaneSlots(Pane.At(0), Pane.Gone, Pane.At(2)),
			searchWindow(depth = 2, panes = 2, levels = 3),
		)
	}

	@Test
	fun `search keeps the results at every width above one pane`() {
		for (depth in 0..2) {
			for (panes in 2..3) {
				val slots = searchWindow(depth, panes, levels = 3)
				assertEquals("depth=$depth panes=$panes", Pane.At(0), slots.list)
			}
		}
	}

	@Test
	fun `search at one pane shows only the level being read`() {
		assertEquals(
			PaneSlots(Pane.Gone, Pane.At(1), Pane.Gone),
			searchWindow(depth = 1, panes = 1, levels = 3),
		)
	}

	@Test
	fun `search pads unclaimed room with blank panes`() {
		// Nothing chosen: results, one labelled placeholder, and a blank -
		// not a second placeholder, and not a missing third of the width.
		assertEquals(
			PaneSlots(Pane.At(0), Pane.Waiting, Pane.Blank),
			searchWindow(depth = 0, panes = 3, levels = 3),
		)
	}

	@Test
	fun `every visible window is full`() {
		// What paneDirective's equal division divides by is the pane count,
		// which is only exact because a window never has fewer non-Gone slots
		// than panes - widths must depend on the window and the tab alone,
		// never on how far the user has drilled in.
		for (levels in 2..3) {
			for (depth in 0 until levels) {
				for (panes in 1..levels) {
					val rules = listOf<(Int, Int, Int) -> PaneSlots>(::leadingWindow, ::searchWindow)
					for (rule in rules) {
						val slots = rule(depth, panes, levels)
						val shown = listOf(slots.list, slots.detail, slots.extra)
							.count { it !is Pane.Gone }
						assertEquals(
							"levels=$levels depth=$depth panes=$panes",
							panes,
							shown,
						)
					}
				}
			}
		}
	}

	@Test
	fun `every width shows the current level`() {
		for (levels in 2..3) {
			for (depth in 0 until levels) {
				// The strip never asks for more panes than the tab has levels.
				for (panes in 1..levels) {
					val rules = listOf<(Int, Int, Int) -> PaneSlots>(::leadingWindow, ::searchWindow)
					for (rule in rules) {
						val slots = rule(depth, panes, levels)
						val shown = listOf(slots.list, slots.detail, slots.extra)
						assertTrue(
							"levels=$levels depth=$depth panes=$panes gave $slots",
							Pane.At(depth) in shown,
						)
					}
				}
			}
		}
	}
}
