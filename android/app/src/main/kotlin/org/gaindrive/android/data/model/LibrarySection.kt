package org.gaindrive.android.data.model

/**
 * Which part of the library a listing, a mirror row or a sort preference
 * belongs to.
 *
 * A closed enum where its predecessor (`LibraryMode`) was an open value class.
 * The openness existed for the chip row, whose chips came from the server and
 * so could name a kind this build had never heard of. With the chips gone
 * there is nothing left to draw for an unknown kind — the merged list has
 * exactly two groups, and uploads is its own screen — so a root typed with
 * something new simply contributes nothing until the app learns what it means.
 *
 * [id] is the string on the wire (`contentType`), in the mirror's
 * `artists.contentType` column, and in the `album_sort_<id>` preference keys.
 * The old `folder:<name>` values have no successor: an untyped server's roots
 * all merge into the [ARTISTS] group now, however many it has.
 */
enum class LibrarySection(val id: String, val label: String) {

	/**
	 * Performers. Also where every root of a server that names no kind of root
	 * lands — see `data/browse/LibraryRoots.kt`.
	 */
	ARTISTS("artists", "Artists"),

	/**
	 * Sections rather than performers — Film, Series. Named separately because
	 * downstream screens key on it: a section has no portrait and no
	 * biography, and the server refuses to look one up.
	 */
	CATEGORIES("categories", "Categories"),

	/**
	 * The account's own upload area. Not a kind of root at all: the server
	 * keeps the uploads root out of `getMusicFolders`, and the slice reaches
	 * the wire as `personal=true` rather than as a `contentType`. It is a
	 * section like the others below that line, which is why the mirror needed
	 * no schema change — `artists.contentType` simply holds "uploads".
	 */
	UPLOADS("uploads", "Uploads"),
}

/**
 * The Library screen's one merged list: every category folder from every
 * `categories` root first, then the usual artist index buckets.
 *
 * [categories] is flat rather than bucketed — the whole group sits under a
 * single "Categories" header, since a library has a handful of sections, not
 * hundreds. [artists] keeps the A/B/C… buckets the alphabet rail scrubs.
 */
data class LibraryListing(
	val categories: List<Artist>,
	val artists: List<ArtistIndex>,
) {
	val isEmpty: Boolean get() = categories.isEmpty() && artists.isEmpty()
}
