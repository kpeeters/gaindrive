package org.gaindrive.android.data

import org.gaindrive.android.data.model.Artist
import org.gaindrive.android.data.model.ArtistIndex

/**
 * The merge rules for "All servers" scope, chosen to be predictable rather than
 * clever. Only artists merge:
 *
 * * Artists collapse when their names match after case-folding and trimming.
 *   The merged row sums the album counts and remembers every contributing ref.
 * * Albums do not merge. An album held on two servers is two rows, each badged.
 *   Merging them would mean reconciling differing track lists, editions and
 *   encodings, and silently hiding one of them.
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
