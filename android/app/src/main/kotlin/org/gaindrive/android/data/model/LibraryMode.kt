package org.gaindrive.android.data.model

/**
 * Which slice of the library the top-level list is showing.
 *
 * A value class over a plain string rather than an enum, deliberately. The
 * slices on offer come from the server, so an enum would have to be edited every
 * time a server grows a new kind of root and would have no sensible branch for
 * one it had never heard of.
 *
 * A list must never mix slices, which is why this is a single value rather than
 * a set: the server filters on it and the resulting list is single-slice by
 * construction.
 *
 * The string names one of two different things, distinguished by a prefix:
 *
 * * a **content type** — `artists`, `categories` — which is a gaindrive
 *   extension naming a *kind* of root, and may span several of them
 * * a **music folder**, written `folder:<name>`, which is one root of a server
 *   that has no concept of kinds
 *
 * Keeping both in one string is what lets `SettingsStore.libraryMode` and the
 * mirror's `contentType` column carry either without a migration, and what makes
 * a value written by an older build still parse. A folder is keyed on its
 * **name** rather than its id because ids are per-server and the chip row unions
 * several servers — the same reason `Merge.kt` matches everything else by name.
 *
 * Personal uploads are not a slice here, although a URL shared with the app now
 * lands in them — see `ui/fetch/`. Browsing them is the part still missing, and
 * a chip is not the whole of it: the listing needs `personal=true` on every
 * hierarchy query and an admin-only promote action, none of which the merge and
 * mirror layers below have a notion of yet.
 */
@JvmInline
value class LibraryMode(val id: String) {

	/** The folder this names, or null when it names a content type. */
	val folderName: String?
		get() = if (id.startsWith(FOLDER)) id.removePrefix(FOLDER) else null

	/**
	 * Title-cased for display. A folder shows its own name, which the server's
	 * owner chose and is already how they think of it. A content type falls back
	 * to capitalising the raw id, so a kind this app has never seen still shows
	 * something sensible rather than nothing.
	 */
	val label: String
		get() = folderName ?: (LABELS[id] ?: id.replaceFirstChar { it.uppercase() })

	companion object {
		/**
		 * What a server that names no kind of root is taken to hold — see
		 * `data/browse/LibraryRoots.kt`.
		 */
		val ARTISTS = LibraryMode("artists")

		/**
		 * A folder named "artists" is `folder:artists`, so it cannot be mistaken
		 * for the content type of the same name.
		 */
		fun folder(name: String) = LibraryMode("$FOLDER$name")

		private const val FOLDER = "folder:"

		private val LABELS = mapOf(
			"artists" to "Artists",
			"categories" to "Categories",
		)
	}
}
