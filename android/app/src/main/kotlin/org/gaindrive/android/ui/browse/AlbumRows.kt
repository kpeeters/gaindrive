package org.gaindrive.android.ui.browse

import org.gaindrive.android.data.model.Chapter
import org.gaindrive.android.data.model.ItemRef
import org.gaindrive.android.data.model.Song

/**
 * The album screen's track listing, flattened to one entry per row.
 *
 * It exists because a chaptered recording is not one row. Its markers stand in
 * for it — a concert is one file holding a dozen songs, and listing it as
 * `concert.mkv` names the file rather than the music — so one song can produce
 * many rows, and the screen's "one lazy item per song" shape no longer holds.
 * Precomputing here rather than nesting the markers inside their song's item is
 * what keeps every row individually keyed and recyclable: the server allows a
 * thousand markers on one item.
 *
 * Pure, so the rules below can be tested without a screen or a player.
 */
sealed interface AlbumListRow {
	/**
	 * Headings drawn above this row, in order, inside its own item.
	 *
	 * A list rather than a single heading, and that is not generality for its
	 * own sake: a chaptered recording emits no row of its own, so when it is
	 * the first item of disc 2 there is nothing left to hang "Disc 2" on and
	 * both it and the recording's name land on the first marker. Keeping them
	 * with the row also keeps the keys one per row.
	 */
	val headings: List<String>

	/** Stable across a re-fetch, so nothing re-composes when chapters arrive. */
	val key: String

	data class Track(
		override val headings: List<String>,
		val song: Song,
		/** Where this song is in the album, which is what playback queues from. */
		val queueIndex: Int,
		val number: Int?,
	) : AlbumListRow {
		override val key: String get() = song.ref.encode()
	}

	data class Marker(
		override val headings: List<String>,
		/** The recording the marker is inside, which is what actually plays. */
		val song: Song,
		val queueIndex: Int,
		val chapter: Chapter,
	) : AlbumListRow {
		override val key: String get() = "c/${song.ref.encode()}/${chapter.index}"
	}
}

/**
 * Flattens an album's songs, replacing each chaptered recording with its
 * markers.
 *
 * Three rules, all of them the web client's:
 *
 * * a disc heading appears only when the album spans more than one disc, since
 *   a heading says *which* group a row belongs to and one group needs none. It
 *   reads "Series 2" rather than "Disc 2" when the server says that group is a
 *   season — checked per group, because a show's unnumbered `Specials` folder
 *   genuinely is a disc.
 * * a heading naming the recording appears only when **more than one** item in
 *   the album has markers, by exactly the same argument. Counted over what will
 *   actually be drawn, so an empty entry cannot conjure one.
 * * an album whose tracks are all numbered 0 or 1 carries no usable numbering,
 *   so its rows are numbered by position instead.
 *
 * [chapters] absent or empty produces precisely the listing this screen drew
 * before chapters existed, which is what makes the feature free for the albums
 * that have none.
 */
fun albumListRows(
	songs: List<Song>,
	chapters: Map<ItemRef, List<Chapter>>,
): List<AlbumListRow> {
	val multiDisc = songs.mapTo(mutableSetOf()) { it.discNumber ?: 1 }.size > 1
	val useSeq = songs.all { (it.track ?: 0) <= 1 }
	val multiChaptered = songs.count { chapters[it.ref]?.isNotEmpty() == true } > 1

	val rows = mutableListOf<AlbumListRow>()
	// Accumulated rather than emitted: whichever row comes next carries them,
	// and for a chaptered recording that row is its first marker.
	var pending = mutableListOf<String>()
	var currentDisc: Int? = null

	songs.forEachIndexed { index, song ->
		val disc = song.discNumber ?: 1
		if (multiDisc && disc != currentDisc) {
			currentDisc = disc
			pending.add("${if (song.season != null) "Series" else "Disc"} $disc")
		}

		val markers = chapters[song.ref].orEmpty()
		if (markers.isEmpty()) {
			rows.add(
				AlbumListRow.Track(
					headings = pending,
					song = song,
					queueIndex = index,
					number = if (useSeq) index + 1 else song.track,
				)
			)
			pending = mutableListOf()
			return@forEachIndexed
		}

		if (multiChaptered) pending.add(song.title)
		markers.forEach { chapter ->
			rows.add(
				AlbumListRow.Marker(
					headings = pending,
					song = song,
					queueIndex = index,
					chapter = chapter,
				)
			)
			pending = mutableListOf()
		}
	}
	return rows
}
