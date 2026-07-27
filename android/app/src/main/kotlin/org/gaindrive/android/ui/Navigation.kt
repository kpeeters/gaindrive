package org.gaindrive.android.ui

import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.automirrored.filled.QueueMusic
import androidx.compose.material.icons.filled.History
import androidx.compose.material.icons.filled.Person
import androidx.compose.material.icons.filled.Search
import androidx.compose.material.icons.filled.Settings
import androidx.compose.ui.graphics.vector.ImageVector
import kotlinx.serialization.Serializable

/**
 * Type-safe navigation routes.
 *
 * Detail routes carry an encoded [org.gaindrive.android.data.model.ItemRef]
 * rather than a bare id — a bare id would be ambiguous the moment a second
 * server is configured. The display name travels alongside so the app bar has
 * something to show before the body has loaded.
 */
sealed interface Route {

	@Serializable
	data object Artists : Route

	@Serializable
	data class Albums(val artistRef: String, val artistName: String) : Route

	@Serializable
	data class Album(val albumRef: String, val albumTitle: String) : Route

	@Serializable
	data object Playlists : Route

	@Serializable
	data object Recents : Route

	@Serializable
	data object Search : Route

	@Serializable
	data object Settings : Route

	/** Null [serverId] means "add a server" — the same screen serves both. */
	@Serializable
	data class ServerEdit(val serverId: String? = null) : Route
}

/** The bottom navigation destinations, in bar order. */
enum class TopLevel(
	val route: Route,
	val label: String,
	val icon: ImageVector,
) {
	ARTISTS(Route.Artists, "Artists", Icons.Default.Person),
	PLAYLISTS(Route.Playlists, "Playlists", Icons.AutoMirrored.Filled.QueueMusic),
	RECENTS(Route.Recents, "Recents", Icons.Default.History),
	SEARCH(Route.Search, "Search", Icons.Default.Search),
	SETTINGS(Route.Settings, "Settings", Icons.Default.Settings),
}
