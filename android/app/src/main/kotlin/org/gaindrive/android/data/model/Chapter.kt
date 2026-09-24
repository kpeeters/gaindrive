package org.gaindrive.android.data.model

import kotlin.math.roundToLong

/**
 * The song boundaries inside one long recording.
 *
 * A concert, a DJ set or a fetched mixtape is one file holding a dozen songs,
 * and these are what let a client say where each starts. They live in a sidecar
 * text file beside the media on the server, indexed by its scan, and reach us
 * through `getChapters`, `getAlbumChapters` and a `chapter` array on the search
 * endpoints. See `doc/api.toml` at the repository root.
 *
 * These are the one library concept with **no [ItemRef]**, against the rule
 * stated at the top of `Library.kt`, and that is the server's rule rather than
 * an omission here: a chapter has no id of its own, cannot be streamed, starred
 * or queued, and is acted on by playing the song it is inside and seeking to
 * its start. Hence a file of their own, where the exception can be argued once.
 *
 * The client is read-only. `getChapters` also reports whether the caller may
 * save markers for an item; it is deliberately not carried into these models,
 * because a field nothing reads is the promise of an editor that does not
 * exist.
 */
data class Chapter(
	/** Position in the list, from 1, as the server numbers it after its sort. */
	val index: Int,
	/**
	 * Seconds from the start of the file, to the millisecond.
	 *
	 * Kept as sent rather than rounded to whole seconds: the server reports this
	 * precisely so that a client reading a list and writing it back is a fixed
	 * point, and truncating here would be the first step in losing that.
	 */
	val startSeconds: Double,
	/**
	 * Whole seconds until the next marker, or until the end of the file.
	 *
	 * Derived by the server, which is the only side that knows the item's own
	 * length. **0** when that span is not positive, which is what a marker past
	 * the end of the file gives and what two markers on one timestamp give.
	 * Neither is an error - a hand-typed file is allowed to be wrong.
	 */
	val duration: Int,
	/** The title on that line, which may be empty. See [displayName]. */
	val name: String,
) {
	val startMs: Long get() = (startSeconds * 1000).roundToLong()

	/**
	 * What to draw for a marker somebody left bare.
	 *
	 * The placeholder is the client's, never the server's: it reports an empty
	 * name as empty on purpose, so that a client saving back what it read cannot
	 * write "Chapter 3" into a line deliberately left blank.
	 */
	val displayName: String get() = chapterLabel(name, index)
}

/** Everything `getChapters` answers about one item. */
data class ChapterList(
	val chapters: List<Chapter> = emptyList(),
	val source: ChapterSource = ChapterSource.NONE,
) {
	val isEmpty: Boolean get() = chapters.isEmpty()
}

/** Where a marker list came from. */
enum class ChapterSource {
	/** A sidecar file beside the media. Wins outright when both exist. */
	SIDECAR,

	/**
	 * Read out of the container itself, which is reachable for a video only.
	 * Worth telling the reader, because such a list is **not** in the scan's
	 * index and so does not appear in the album listing.
	 */
	CONTAINER,

	/** No sidecar and nothing in the container, or the file could not be read. */
	NONE;

	companion object {
		fun from(wire: String): ChapterSource = when (wire) {
			"sidecar" -> SIDECAR
			"container" -> CONTAINER
			else -> NONE
		}
	}
}

/** One chaptered item in an album folder, as `getAlbumChapters` lists it. */
data class SongChapters(
	val ref: ItemRef,
	val title: String,
	val chapters: List<Chapter>,
)

/**
 * A marker whose title matched a search.
 *
 * A different shape from [Chapter] rather than the same one with holes, and the
 * difference is not merely a missing field. A search hit *is* its own context -
 * it names the recording, the album and the artist, because a marker means
 * nothing without knowing which concert it is in - while a [Chapter] is always
 * read alongside the item the caller already holds. It also carries no
 * duration, which cannot be known without the rest of the list, so folding the
 * two together would make `duration == 0` mean "no next marker" in one case and
 * "unknowable" in the other.
 */
data class ChapterHit(
	/** The song this marker is inside. Play *that* and seek to [startMs]. */
	val songRef: ItemRef,
	/** That song's album folder, so a client can open the listing it sits in. */
	val albumRef: ItemRef?,
	val index: Int,
	val startSeconds: Double,
	val name: String,
	/** The title of the song or film the marker is inside. */
	val trackTitle: String,
	val albumTitle: String,
	val artistName: String,
) {
	val startMs: Long get() = (startSeconds * 1000).roundToLong()
	val displayName: String get() = chapterLabel(name, index)
}

private fun chapterLabel(name: String, index: Int): String =
	name.ifBlank { "Chapter $index" }

// ── Navigating a marker list ────────────────────────────────────────────────
//
// Pure functions over a sorted list, so the rules can be tested without a
// player. They are ported from the web client rather than reinvented: two
// clients disagreeing about which song is playing would be worse than either
// rule on its own.

/**
 * A marker counts as reached slightly early.
 *
 * Without the tolerance, seeking to a marker frequently lands a few
 * milliseconds short of it - the player rounds, and a re-encoded stream starts
 * at the nearest keyframe - so the list would highlight the *previous* song for
 * a moment after jumping to one.
 */
const val CHAPTER_TOLERANCE_MS = 250L

/**
 * How far into a chapter "previous" stops meaning "restart this one".
 *
 * The behaviour every physical transport has, and the reason a viewer can press
 * it twice to go back a song.
 */
const val CHAPTER_RESTART_MS = 3_000L

/** Index of the marker being played at [positionMs], or -1 before the first. */
fun List<Chapter>.currentAt(positionMs: Long): Int {
	var found = -1
	for ((i, chapter) in withIndex()) {
		if (chapter.startMs <= positionMs + CHAPTER_TOLERANCE_MS) found = i else break
	}
	return found
}

/** The next marker after [positionMs], or null once past the last one. */
fun List<Chapter>.nextAfter(positionMs: Long): Chapter? =
	firstOrNull { it.startMs > positionMs + CHAPTER_TOLERANCE_MS }

/**
 * Where "previous chapter" should seek to from [positionMs].
 *
 * The start of the chapter being played, unless we are already at it - and 0
 * when nothing has started yet, so the control is never inert.
 */
fun List<Chapter>.previousTargetMs(positionMs: Long): Long {
	val i = currentAt(positionMs)
	if (i < 0) return 0
	val start = this[i].startMs
	return if (i == 0 || positionMs - start > CHAPTER_RESTART_MS) start else this[i - 1].startMs
}
