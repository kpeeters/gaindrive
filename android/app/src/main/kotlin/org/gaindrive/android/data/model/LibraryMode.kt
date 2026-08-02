package org.gaindrive.android.data.model

/**
 * Which kind of top-level entry the library is showing.
 *
 * A value class over a plain string rather than an enum, deliberately. The
 * modes on offer come from the server — one per configured root content type —
 * so an enum would have to be edited every time a server grows a new kind of
 * root, and would have no sensible branch for one it had never heard of.
 *
 * A list must never mix kinds, which is why this is a single value rather than
 * a set: the server filters on it and the resulting list is single-kind by
 * construction.
 *
 * Personal uploads are not a mode here. The server offers them, but this app
 * cannot upload, so the chip would name somewhere nothing can be put from.
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

	companion object {
		/**
		 * What a server that has never heard of root kinds is taken to hold —
		 * see the legacy handling in LibraryRepository.
		 */
		val ARTISTS = LibraryMode("artists")

		private val LABELS = mapOf(
			"artists" to "Artists",
			"categories" to "Categories",
		)
	}
}
