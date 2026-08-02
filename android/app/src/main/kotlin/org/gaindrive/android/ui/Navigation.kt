package org.gaindrive.android.ui

import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.automirrored.filled.QueueMusic
import androidx.compose.material.icons.filled.History
import androidx.compose.material.icons.filled.LibraryMusic
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

	/**
	 * [artistRefs] is comma-separated: a merged artist row stands for the same
	 * artist on several servers, each with its own id, and the albums screen
	 * has to ask all of them.
	 */
	@Serializable
	data class Albums(val artistRefs: String, val artistName: String) : Route

	@Serializable
	data class Album(val albumRef: String, val albumTitle: String) : Route

	@Serializable
	data object Playlists : Route

	@Serializable
	data class Playlist(val playlistRef: String, val playlistName: String) : Route

	@Serializable
	data object Recents : Route

	@Serializable
	data object Search : Route

	@Serializable
	data object Settings : Route

	/**
	 * Settings categories. Nested rather than one flat screen: the list was
	 * already long before casting, metadata editing, user administration and
	 * upload arrive, and this is what Android's own Settings does.
	 */
	@Serializable
	data object SettingsServers : Route

	@Serializable
	data object SettingsLibrary : Route

	@Serializable
	data object SettingsStorage : Route

	@Serializable
	data object SettingsAppearance : Route

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
	// The destination shows artists, categories or uploads depending on the
	// mode chosen in its own top bar, so the label names the place rather than
	// one of the things it can hold. Route.Artists keeps its name to avoid
	// churning navigation for a wording change.
	ARTISTS(Route.Artists, "Library", Icons.Default.LibraryMusic),
	PLAYLISTS(Route.Playlists, "Playlists", Icons.AutoMirrored.Filled.QueueMusic),
	RECENTS(Route.Recents, "Recents", Icons.Default.History),
	SEARCH(Route.Search, "Search", Icons.Default.Search),
	SETTINGS(Route.Settings, "Settings", Icons.Default.Settings),
}
