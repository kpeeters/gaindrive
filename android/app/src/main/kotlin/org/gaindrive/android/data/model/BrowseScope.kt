package org.gaindrive.android.data.model

/**
 * Which servers the browse screens are showing.
 *
 * [AllServers] is the default rather than a named server: with one server
 * configured the two are the same thing and the selector is hidden entirely, so
 * the app still looks like a single-server client until it isn't one. With
 * several, seeing all of them is the reason they were added.
 */
sealed interface BrowseScope {

	data object AllServers : BrowseScope

	data class OneServer(val id: ServerId) : BrowseScope

	companion object {
		/** Stored in place of a server id. Not a UUID, so it cannot collide. */
		const val ALL_STORED = "all"
	}
}
