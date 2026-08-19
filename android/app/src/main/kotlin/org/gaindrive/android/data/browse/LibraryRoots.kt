package org.gaindrive.android.data.browse

import org.gaindrive.android.data.model.LibraryMode
import org.gaindrive.android.data.model.MusicRoot

/**
 * What the chip row offers, and what a chosen chip becomes on the wire.
 *
 * Pure: every decision here is a function of the server's `getMusicFolders`
 * answer and its browse setting, so it is exercised directly rather than through
 * a repository that would need a fake server to run at all.
 */

/** The two ways a top-level listing can be narrowed. Both null means "all". */
data class RootRequest(
	val musicFolderId: String? = null,
	val contentType: String? = null,
)

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
 */
fun chipsFor(browseByFolder: Boolean, roots: List<MusicRoot>): List<LibraryMode> {
	val types = roots.mapNotNull { it.contentType }.distinct()
	if (types.isNotEmpty()) return types.map { LibraryMode(it) }

	if (browseByFolder && roots.size > 1) return roots.map { LibraryMode.folder(it.name) }

	return listOf(LibraryMode.ARTISTS)
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
): RootRequest? {
	val chips = chipsFor(browseByFolder, roots)
	if (mode !in chips) return null

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
 * Content types first, then folders, each alphabetically — so a row mixing the
 * two from different servers has a stable shape rather than one that depends on
 * which server answered first. Deduplicated case-insensitively, keeping the
 * first spelling in registry order, which is the same tie-break every other
 * merge uses.
 */
fun mergeChips(perServer: List<List<LibraryMode>>): List<LibraryMode> {
	val seen = mutableMapOf<String, LibraryMode>()
	perServer.flatten().forEach { seen.putIfAbsent(it.id.lowercase(), it) }
	return seen.values.sortedWith(
		compareBy<LibraryMode> { if (it.folderName == null) 0 else 1 }
			.thenBy(String.CASE_INSENSITIVE_ORDER) { it.label }
	)
}
