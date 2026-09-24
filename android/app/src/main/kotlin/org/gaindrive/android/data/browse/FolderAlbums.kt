package org.gaindrive.android.data.browse

import org.gaindrive.android.net.SongDto

/**
 * Turning an album folder's subdirectories into disc numbers.
 *
 * Pure, taking listings that have already been fetched, so the rule that decides
 * what a disc is can be tested without a server.
 */

/**
 * Past this many subdirectories the thing being listed is not an album - it is
 * a library root someone navigated into. Recursing over all of it would fire a
 * request per entry to build a track list nobody wants.
 */
const val MAX_DISCS = 12

/**
 * Sort key with digit runs zero-padded, so `CD2` comes before `CD10`.
 *
 * Plain lexical order gets multi-disc sets wrong in exactly the case that has
 * more than nine discs, which is where getting it wrong is most visible.
 */
fun naturalKey(raw: String): String =
	Regex("\\d+").replace(raw.lowercase()) { it.value.padStart(10, '0') }

/** Subdirectories in the order their discs should be numbered. */
val DISC_ORDER: Comparator<SongDto> = compareBy { naturalKey(it.title) }

/**
 * One flat, disc-ordered track list from a set of already-sorted disc folders.
 *
 * Two rules that look like losing information and are not:
 *
 * * **The folder's position always wins over the track's own `discNumber`.**
 *   Distrusting the tags is the entire reason someone turned folder browsing on;
 *   a set whose discs all claim to be disc 1 is precisely the library this
 *   exists for.
 * * **`season` is dropped unless every track in *that disc folder* already had
 *   one**, judged per folder rather than across the album. The two fields carry
 *   the same number and `season` only decides whether the group is headed
 *   "Series 2" or "Disc 2", so synthesising one would label a two-CD album a
 *   television series. Keeping it where it was already unanimous preserves a
 *   real series read through folders - including a show with an unnumbered
 *   `Specials` folder, which heads that one group "Disc" and the rest "Series",
 *   exactly as the server does.
 */
fun flattenDiscs(discs: List<List<SongDto>>): List<SongDto> =
	discs.flatMapIndexed { index, songs ->
		val disc = index + 1
		val season = if (songs.isNotEmpty() && songs.all { it.season != null }) disc else null
		songs.map { it.copy(discNumber = disc, season = season) }
	}
