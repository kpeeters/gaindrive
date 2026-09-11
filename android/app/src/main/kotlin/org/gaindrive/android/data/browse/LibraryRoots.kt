package org.gaindrive.android.data.browse

import org.gaindrive.android.data.model.LibrarySection
import org.gaindrive.android.data.model.MusicRoot

/**
 * What one server is asked for the merged library list, and for the uploads
 * listing.
 *
 * Pure: every decision here is a function of the server's `getMusicFolders`
 * answer and its browse setting, so it is exercised directly rather than
 * through a repository that would need a fake server to run at all.
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
 * The one or two requests one server's merged listing is built from. A null
 * half is a group this server lacks — it contributes nothing there, which is
 * not a failure and must not be reported as one. Not asking is the point: a
 * request for a kind a server does not have would either come back empty or,
 * on a server predating library roots, come back as the *entire* library and
 * put the same folders in both groups.
 */
data class ListingRequests(
	val categories: RootRequest?,
	val artists: RootRequest?,
)

/**
 * What to ask [roots]' server for the merged list.
 *
 * A typed server — any root naming a `contentType` — is asked per kind it
 * actually has, because omitting the parameter there answers with every root
 * mixed together, which is exactly what the two-group list exists to avoid.
 * A root typed with a kind this build has never heard of contributes nothing;
 * the old chip row could surface an unknown kind as its own chip, but the
 * merged list has nowhere meaningful to put one.
 *
 * An untyped server has no opinion about what its roots contain, so all of
 * them land in the artists group with one unnarrowed request — including a
 * folder-mode server with several roots, whose per-folder chips this
 * replaced. In ID3 mode it keeps sending `contentType=artists`, which is
 * exactly what this app sent before folder browsing existed and which a
 * server that has never heard of it ignores.
 */
fun listingRequests(browseByFolder: Boolean, roots: List<MusicRoot>): ListingRequests {
	val types = roots.mapNotNull { it.contentType }.distinct()

	val categories =
		if (LibrarySection.CATEGORIES.id in types) {
			RootRequest(contentType = LibrarySection.CATEGORIES.id)
		} else {
			null
		}

	val artists = when {
		types.isEmpty() ->
			if (browseByFolder) RootRequest()
			else RootRequest(contentType = LibrarySection.ARTISTS.id)
		LibrarySection.ARTISTS.id in types ->
			RootRequest(contentType = LibrarySection.ARTISTS.id)
		else -> null
	}

	return ListingRequests(categories, artists)
}

/**
 * The uploads listing. Handled apart from [listingRequests] because it is not
 * a root: sending `contentType=uploads` would narrow the *shared* library to a
 * kind of root no server has, and answer with nothing at all.
 *
 * [isAdmin] widens the slice to every account's. An admin is the only one who
 * can promote an upload into the shared library, so without this a non-admin's
 * upload is visible to its owner and to nobody able to act on it.
 */
fun uploadsRequest(isAdmin: Boolean) = RootRequest(
	personal = if (isAdmin) PersonalScope.ALL else PersonalScope.MINE,
)
