package org.gaindrive.android.data

import org.gaindrive.android.data.model.Album
import org.gaindrive.android.data.model.Artist
import org.gaindrive.android.data.model.ArtistIndex

/**
 * The merge rules for "All servers" scope, chosen to be predictable rather than
 * clever. Only artists merge:
 *
 * * Artists collapse when their names match after case-folding and trimming.
 *   The merged row sums the album counts and remembers every contributing ref.
 * * Albums collapse on artist and title, but only when [mergeDuplicateAlbums]
 *   is on — it is off by default, because the collapse hides one server's copy
 *   of an album behind another's on nothing more than a title match.
 * * Songs never merge; a track list always comes from one album on one server.
 *
 * Every `perServer` argument must arrive in registry order — it is the tie-break
 * for which server's ref, artwork and index letter a merged row takes.
 */
fun mergeArtists(perServer: List<List<Artist>>): List<Artist> {
	// One server is not a merge. Returning it untouched also preserves the
	// order it chose, which for search results is relevance.
	perServer.singleOrNull()?.let { return it }
	return fold(perServer.map { list -> list.map { NO_LABEL to it } }).map { it.second }
}

/**
 * The index buckets from every server, merged into one set.
 *
 * Artists are keyed across *all* buckets rather than within each: two servers
 * that file the same artist under different letters would otherwise produce two
 * rows, which is exactly what merging is meant to prevent.
 */
fun mergeArtistIndexes(perServer: List<List<ArtistIndex>>): List<ArtistIndex> {
	perServer.singleOrNull()?.let { return it }

	val labelled = perServer.map { indexes ->
		indexes.flatMap { bucket -> bucket.artists.map { bucket.label to it } }
	}

	return fold(labelled)
		.groupBy({ it.first }, { it.second })
		.map { (label, artists) ->
			// Sorted because the merged order is ours to choose: each server
			// sorted its own list, and interleaving two sorted lists by hand
			// is not sorted.
			ArtistIndex(
				label = label,
				artists = artists.sortedWith(compareBy(String.CASE_INSENSITIVE_ORDER) { it.name }),
			)
		}
		.sortedWith(compareBy(LABEL_ORDER) { it.label })
}

/**
 * Collapses copies of the same album held on more than one server, keeping the
 * copy from the server highest in registry order. That order is the user's
 * stated preference — it is what the Settings list reorders — so "my own server
 * before Bandcamp" is expressed by putting it first rather than by a separate
 * favourite-server setting that could disagree with it.
 *
 * Matched on artist and title, case-folded and trimmed. Deliberately *not* on
 * year: a remaster or a re-release disagrees about it between servers, which
 * would split exactly the pairs worth collapsing. The cost is that two genuinely
 * different albums sharing a title under one artist collapse into one — hence
 * the setting that turns this off, and the badges that keep the survivors
 * honest about who else has a copy.
 *
 * [albums] must arrive in registry order.
 */
fun mergeAlbums(albums: List<Album>): List<Album> {
	// Across servers only. Two same-titled albums on one server are two albums
	// — separately filed editions — and collapsing them would delete a row the
	// user's own library deliberately has twice.
	if (albums.distinctBy { it.ref.server }.size < 2) return albums

	val merged = LinkedHashMap<Pair<String, String>, Album>()
	albums.forEach { album ->
		val key = album.artistName.trim().lowercase() to album.title.trim().lowercase()
		val existing = merged[key]
		merged[key] = if (existing == null) {
			album
		} else {
			existing.copy(
				refs = existing.refs + album.refs,
				// Starred anywhere is starred, as for artists.
				starredAt = existing.starredAt ?: album.starredAt,
			)
		}
	}
	return merged.values.toList()
}

/**
 * Collapses same-named artists, keeping first-seen order. The first
 * contributor — earliest server in registry order — wins the ref, artwork and
 * index letter; only the album count and the ref list grow.
 */
private fun fold(perServer: List<List<Pair<String, Artist>>>): List<Pair<String, Artist>> {
	val merged = LinkedHashMap<String, Pair<String, Artist>>()
	perServer.forEach { entries ->
		entries.forEach { (label, artist) ->
			val key = artist.name.trim().lowercase()
			val existing = merged[key]
			merged[key] = if (existing == null) {
				label to artist
			} else {
				val (firstLabel, first) = existing
				firstLabel to first.copy(
					albumCount = first.albumCount + artist.albumCount,
					refs = first.refs + artist.refs,
					// Starred anywhere is starred: the alternative is a row
					// whose star depends on which server answered first.
					starredAt = first.starredAt ?: artist.starredAt,
				)
			}
		}
	}
	return merged.values.toList()
}

/** Search results carry no index letter; the label is unused there. */
private const val NO_LABEL = ""

/**
 * Letters first, alphabetically, then everything else — "#" belongs at the end
 * of the rail, not in front of "A" where its ASCII value would put it.
 */
private val LABEL_ORDER = Comparator<String> { a, b ->
	val aLetter = a.firstOrNull()?.isLetter() == true
	val bLetter = b.firstOrNull()?.isLetter() == true
	when {
		aLetter && !bLetter -> -1
		!aLetter && bLetter -> 1
		else -> String.CASE_INSENSITIVE_ORDER.compare(a, b)
	}
}
