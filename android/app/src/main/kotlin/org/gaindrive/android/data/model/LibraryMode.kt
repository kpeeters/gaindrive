package org.gaindrive.android.data.model

/**
 * Which kind of top-level entry the library is showing.
 *
 * A value class over a plain string rather than an enum, deliberately. The
 * modes on offer come from the server — one per configured root content type —
 * so an enum would have to be edited every time a server grows a new kind of
 * root, and would have no sensible branch for one it had never heard of. The
 * only value this app names itself is [UPLOADS], which is not a library root
 * at all but the user's own space, reached through a different parameter.
 *
 * A list must never mix kinds, which is why this is a single value rather than
 * a set: the server filters on it and the resulting list is single-kind by
 * construction.
 */
@JvmInline
value class LibraryMode(val id: String) {

	/**
	 * Title-cased for display. Falls back to capitalising the raw id, so a
	 * content type this app has never seen still shows something sensible
	 * rather than nothing.
	 */
	val label: String
		get() = LABELS[id] ?: id.replaceFirstChar { it.uppercase() }

	/** Uploads are fetched with `personal=true`, not with a content type. */
	val isUploads: Boolean get() = id == UPLOADS.id

	companion object {
		val ARTISTS = LibraryMode("artists")
		val UPLOADS = LibraryMode("uploads")

		private val LABELS = mapOf(
			"artists" to "Artists",
			"categories" to "Categories",
			"uploads" to "Uploads",
		)
	}
}
