package org.gaindrive.android.ui.adaptive

import androidx.compose.runtime.Immutable
import kotlin.math.max

/*
 * Which panes are on screen, and what each of them holds.
 *
 * Pure functions of "how deep is this tab, how many panes fit, and how many
 * levels does it have" - no Compose, no layout, nothing to run. That is
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

	/**
	 * On screen, for a level deeper than the one [Waiting] invites - nothing
	 * can be chosen at it yet, and unlike Waiting it says nothing, because a
	 * label there would invite a choice that cannot be made. It exists to
	 * hold its width: with it, every visible window is full (non-[Gone] slots
	 * == `min(panes, levels)`), so the strip's widths are a function of the
	 * window and the tab alone and never jump as the user drills in. It is
	 * the web client's blank pane `<div>`, given a name.
	 */
	data object Blank : Pane

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
 * [levels] is how deep the tab goes - two for Playlists, three for
 * the Library - and bounds both the window and the placeholders, so a tab with
 * nothing at level 2 never draws a third pane inviting a choice that does not
 * exist.
 */
fun leadingWindow(depth: Int, panes: Int, levels: Int): PaneSlots {
	val leftmost = max(0, depth - (panes - 1))
	fun at(level: Int): Pane = when {
		level < leftmost || level > leftmost + panes - 1 || level >= levels -> Pane.Gone
		level <= depth -> Pane.At(level)
		// Only the level immediately below the one being read is *labelled*. A
		// third pane asking for an album while no artist is chosen invites a
		// choice that cannot be made, so anything deeper is Blank - on screen
		// for its width, saying nothing, exactly as the web client draws it.
		level == depth + 1 -> Pane.Waiting
		else -> Pane.Blank
	}
	return PaneSlots(at(0), at(1), at(2))
}

/**
 * Search's mapping, and the one exception to [leadingWindow].
 *
 * A result list is not a level you pass through on the way somewhere - it is
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
		// Results, then the deepest levels that still fit - and a placeholder
		// only in room nothing occupied has claimed.
		val room = panes - 1
		val shown = (1..depth).toList().takeLast(room)
		val spare = if (shown.size < room && depth + 1 <= levels - 1) listOf(depth + 1) else emptyList()
		setOf(0) + shown + spare
	}
	// Room the spare could not claim is filled with blank levels below it, so
	// the window is as full here as under leadingWindow and the widths match.
	val blank: Set<Int> = ((depth + 2) until levels).take(panes - visible.size).toSet()
	fun at(level: Int): Pane = when {
		level in blank -> Pane.Blank
		level !in visible -> Pane.Gone
		level <= depth -> Pane.At(level)
		else -> Pane.Waiting
	}
	return PaneSlots(at(0), at(1), at(2))
}
