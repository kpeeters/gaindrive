package org.gaindrive.android.data.browse

import org.gaindrive.android.data.model.LibraryMode
import org.gaindrive.android.data.model.MusicRoot

/**
 * What the chip row offers, and what a chosen chip becomes on the wire.
 *
 * Pure: every decision here is a function of the server's `getMusicFolders`
 * answer, its browse setting and whether this account may upload to it, so it is
 * exercised directly rather than through a repository that would need a fake
 * server to run at all.
 */

/**
 * Whose uploads a listing covers, if anyone's.
 *
 * An enum rather than two booleans: exactly one of these is true at a time, and
 * a pair could be set to a combination that means nothing.
 */
enum class PersonalScope(
	/**
	 * What the wire wants, or null to send no parameter at all.
	 *
	 * [NONE] is deliberately null rather than "false". The server tests for the
	 * exact strings, so "false" would work — but it would also append a
	 * parameter to every ordinary library request that never carried one, which
	 * is a gratuitous difference from what a third-party server has always seen.
	 * Retrofit omits a null entirely.
	 */
	val param: String?,
) {
	/** The shared library. */
	NONE(null),

	/** This account's own uploads. */
	MINE("true"),

	/**
	 * Every account's uploads, which the server allows only for an admin.
	 *
	 * It also groups the response by owner instead of by first letter, so the
	 * index labels come back as usernames — which is what makes two people's
	 * identically named folders tellable apart, and needs no client change
	 * because a label was always just a string.
	 */
	ALL("*"),
}

/**
 * How a top-level listing is narrowed. All three at their defaults means "the
 * whole shared library".
 *
 * [personal] is not a third way of naming a root — it switches to a different
 * library altogether, and the server ignores the other two while it is set.
 * Kept in the same object regardless, because every caller wants exactly one of
 * these and a second parameter alongside would let them be passed
 * inconsistently.
 */
data class RootRequest(
	val musicFolderId: String? = null,
	val contentType: String? = null,
	val personal: PersonalScope = PersonalScope.NONE,
) {
	val personalParam: String? get() = personal.param
}

/**
 * The chips one server contributes.
 *
 * Three cases, tested in this order:
 *
 * 1. **Any root names a content type** — the server understands kinds of root,
 *    so the chips are those kinds. This is tested *first*, which is what makes
 *    gaindrive behave identically in both browse modes: the switch changes which
 *    endpoints are called and nothing else. Note the kinds are distinct values,
 *    not one per root — two artist roots are one "Artists" chip, because a kind
 *    may span several roots and the server filters on the kind.
 * 2. **Folder mode with more than one untyped root** — the server has several
 *    libraries and no opinion about what they contain, so each becomes a chip.
 * 3. **Anything else** — one Artists chip, which with a single server means no
 *    chip row is drawn at all. The threshold in case 2 is what keeps it that
 *    way for the common single-library server: growing a one-chip row would
 *    change how the app looks for someone who gained nothing by it.
 *
 * [canUpload] appends the Uploads chip to whichever of those applies, and is a
 * fact about the *account* rather than about the roots — the server keeps its
 * uploads root out of `getMusicFolders` entirely, so no amount of reading
 * [roots] would ever produce it. It is appended last in all three cases, and it
 * is the one chip that can turn a would-be single-chip row into a real one:
 * that is a row someone gained something by, unlike the case-2 threshold above.
 */
fun chipsFor(
	browseByFolder: Boolean,
	roots: List<MusicRoot>,
	canUpload: Boolean = false,
): List<LibraryMode> {
	val uploads = if (canUpload) listOf(LibraryMode.UPLOADS) else emptyList()

	val types = roots.mapNotNull { it.contentType }.distinct()
	if (types.isNotEmpty()) return types.map { LibraryMode(it) } + uploads

	if (browseByFolder && roots.size > 1) {
		return roots.map { LibraryMode.folder(it.name) } + uploads
	}

	return listOf(LibraryMode.ARTISTS) + uploads
}

/**
 * What to send this server for [mode], or **null when it cannot answer for it
 * at all** — a chip contributed by a different server, which is not a failure
 * and must not be reported as one. Not asking is the point: a server predating
 * library roots ignores an unknown parameter and answers with its entire
 * library, so the request itself is what would put the same artists under every
 * chip.
 */
fun rootRequest(
	browseByFolder: Boolean,
	roots: List<MusicRoot>,
	mode: LibraryMode,
	canUpload: Boolean = false,
	/**
	 * Whether this account administers the server, which widens the Uploads
	 * slice to every account's. An admin is the only one who can promote an
	 * upload into the shared library, so without this a non-admin's upload is
	 * visible to its owner and to nobody able to act on it.
	 */
	isAdmin: Boolean = false,
): RootRequest? {
	val chips = chipsFor(browseByFolder, roots, canUpload)
	if (mode !in chips) return null

	// Handled before everything below it, because it is not a root: sending
	// `contentType=uploads` would narrow the *shared* library to a kind of root
	// no server has, and answer with nothing at all.
	if (mode == LibraryMode.UPLOADS) {
		return RootRequest(
			personal = if (isAdmin) PersonalScope.ALL else PersonalScope.MINE,
		)
	}

	mode.folderName?.let { name ->
		val root = roots.firstOrNull { it.name.equals(name, ignoreCase = true) } ?: return null
		return RootRequest(musicFolderId = root.id)
	}

	// A content-type chip on a server that names no types is the fallback
	// Artists chip from case 3. In ID3 mode it keeps sending
	// `contentType=artists`, which is exactly what this app sent before folder
	// browsing existed and which a server that has never heard of it ignores.
	// In folder mode there is nothing to narrow by, so neither parameter goes.
	val types = roots.mapNotNull { it.contentType }
	if (types.isEmpty() && browseByFolder) return RootRequest()
	return RootRequest(contentType = mode.id)
}

/**
 * The chips of every server in scope, as one row.
 *
 * Content types first, then folders, then Uploads, each group alphabetically —
 * so a row mixing them from different servers has a stable shape rather than one
 * that depends on which server answered first. Deduplicated case-insensitively,
 * keeping the first spelling in registry order, which is the same tie-break
 * every other merge uses.
 *
 * Uploads is ranked last explicitly rather than left to sort alphabetically
 * among the content types, where it happens to land after "Artists" and
 * "Categories" today and would land before a server's "Videos" tomorrow. It is
 * not one of the library's slices — it is the user's own corner of the server —
 * so it belongs at the end whatever it is spelled next to.
 */
fun mergeChips(perServer: List<List<LibraryMode>>): List<LibraryMode> {
	val seen = mutableMapOf<String, LibraryMode>()
	perServer.flatten().forEach { seen.putIfAbsent(it.id.lowercase(), it) }
	return seen.values.sortedWith(
		compareBy<LibraryMode> {
			when {
				it == LibraryMode.UPLOADS -> 2
				it.folderName != null -> 1
				else -> 0
			}
		}.thenBy(String.CASE_INSENSITIVE_ORDER) { it.label }
	)
}
