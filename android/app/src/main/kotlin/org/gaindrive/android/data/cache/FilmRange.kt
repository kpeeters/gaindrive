package org.gaindrive.android.data.cache

import org.gaindrive.android.data.model.Chapter
import org.gaindrive.android.data.model.ItemRef

/**
 * A stretch of a film's soundtrack, which is what a chapter pin keeps.
 *
 * Identified by its bounds rather than by a chapter index, so editing the
 * chapters on the server never changes what an existing pin holds. A chapter
 * row finds its pin by building the range from its own bounds.
 *
 * It travels as an [ItemRef] whose id is `<songId>#<startMs>-<endMs>`, which is
 * what lets it sit in the pin table, in `DownloadStates` and in the
 * availability sets beside ordinary songs. That ref must never reach the
 * library DAO: no row has that id, so a query for it would come back empty
 * rather than fail.
 */
data class FilmRange(val film: ItemRef, val startMs: Long, val endMs: Long) {

	val ref: ItemRef get() = ItemRef(film.server, "${film.id}$SEP$startMs-$endMs")

	val durationMs: Long get() = endMs - startMs

	companion object {
		/** A Subsonic id is an integer, so it cannot contain this. */
		private const val SEP = '#'

		/**
		 * One range per marker, each ending where the next starts and the last
		 * at the end of the film. In one place because a stored chapter is
		 * found by its exact bounds: two derivations that drift apart would
		 * silently stop finding it.
		 */
		fun ofMarkers(film: ItemRef, markers: List<Chapter>, filmDurationMs: Long) =
			markers.mapIndexed { i, chapter ->
				val endMs = markers.getOrNull(i + 1)?.startMs ?: filmDurationMs
				FilmRange(film, chapter.startMs, endMs)
			}

		/** Null for an ordinary ref, which is how a song is told from a range. */
		fun of(ref: ItemRef): FilmRange? {
			val cut = ref.id.indexOf(SEP)
			if (cut <= 0) return null
			val bounds = ref.id.substring(cut + 1).split('-')
			if (bounds.size != 2) return null
			val start = bounds[0].toLongOrNull() ?: return null
			val end = bounds[1].toLongOrNull() ?: return null
			if (start < 0 || end <= start) return null
			return FilmRange(ItemRef(ref.server, ref.id.substring(0, cut)), start, end)
		}
	}
}
