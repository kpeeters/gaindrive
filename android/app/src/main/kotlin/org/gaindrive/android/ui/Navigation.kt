package org.gaindrive.android.ui

import kotlinx.serialization.Serializable

/**
 * Type-safe navigation routes. Phase 1 has only the settings surfaces and a
 * placeholder for the library; Phase 2 replaces [Library] with the real
 * browsing destinations and adds the bottom navigation bar.
 */
sealed interface Route {

	@Serializable
	data object Library : Route

	@Serializable
	data object Settings : Route

	/**
	 * Null [serverId] means "add a server" — the same screen serves both, which
	 * is the whole reason there is no separate login screen.
	 */
	@Serializable
	data class ServerEdit(val serverId: String? = null) : Route
}
