package org.gaindrive.android.ui.adaptive

import androidx.compose.runtime.Immutable
import kotlin.math.max

/*
 * Which panes are on screen, and what each of them holds.
 *
 * Pure functions of "how deep is this tab, how many panes fit, and how many
 * levels does it have" — no Compose, no layout, nothing to run. That is
 * deliberate and matches the rest of the project: the part that can be wrong
 * is the part that can be tested, and `PaneWindowTest` is the whole of the
 * argument that a resize never leaves the current level off screen.
 */

/**
 * What one pane holds.
 *
 * [Gone] and [Waiting] are both "no content", and the difference between them
 * is the whole reason this is a type: a pane the window has slid past must take
 * no width at all, while a pane for a level nothing has been chosen at yet
 * keeps its width and has to say why it is empty. The web client blanks the
 * second case, which is fine for a `<div>` and reads as a rendering fault on a
 * tablet.
 */
@Immutable
sealed interface Pane {
	/** Off the leading edge of the window; not laid out. */
	data object Gone : Pane

	/** On screen, with nothing chosen at this level yet. */
	data object Waiting : Pane

	/** On screen, showing `path[index]`. */
	data class At(val index: Int) : Pane
}

/** The three pane roles, in leading-to-trailing order. */
@Immutable
data class PaneSlots(val list: Pane, val detail: Pane, val extra: Pane)

/**
 * The ordinary mapping, used by every tab but Search: a level *is* a pane role,
 * and the window of visible levels ends at the one being read.
 *
 * [levels] is how deep the tab goes — two for Playlists, three for
 * the Library — and bounds both the window and the placeholders, so a tab with
 * nothing at level 2 never draws a third pane inviting a choice that does not
 * exist.
 */
fun leadingWindow(depth: Int, panes: Int, levels: Int): PaneSlots {
	val leftmost = max(0, depth - (panes - 1))
	fun at(level: Int): Pane = when {
		level < leftmost || level > leftmost + panes - 1 || level >= levels -> Pane.Gone
		level <= depth -> Pane.At(level)
		// Only the level immediately below the one being read. A third pane
		// asking for an album while no artist is chosen invites a choice that
		// cannot be made — the web client can leave both blank because a blank
		// pane says nothing, and a labelled one does.
		level == depth + 1 -> Pane.Waiting
		else -> Pane.Gone
	}
	return PaneSlots(at(0), at(1), at(2))
}

/**
 * Search's mapping, and the one exception to [leadingWindow].
 *
 * A result list is not a level you pass through on the way somewhere — it is
 * the thing being worked from, and dropping it off the leading edge to make
 * room for an album's tracks throws away the query. So level 0 is always on
 * screen above one pane, and it is the *middle* that goes when there is not
 * enough room: results beside the album, rather than the artist beside the
 * album with the search gone. The web client does the same thing by hand, in
 * `viewTracksFromSearch`, which moves the rendered tracks from pane 2 into
 * pane 1 whenever two panes are showing.
 */
fun searchWindow(depth: Int, panes: Int, levels: Int): PaneSlots {
	val visible: Set<Int> = if (panes == 1) {
		setOf(depth)
	} else {
		// Results, then the deepest levels that still fit — and a placeholder
		// only in room nothing occupied has claimed.
		val room = panes - 1
		val shown = (1..depth).toList().takeLast(room)
		val spare = if (shown.size < room && depth + 1 <= levels - 1) listOf(depth + 1) else emptyList()
		setOf(0) + shown + spare
	}
	fun at(level: Int): Pane = when {
		level !in visible -> Pane.Gone
		level <= depth -> Pane.At(level)
		else -> Pane.Waiting
	}
	return PaneSlots(at(0), at(1), at(2))
}
