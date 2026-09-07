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
 * The interface itself is `@Serializable`, not only its members: the pane
 * strip stores a tab's path as a list of routes and needs the sealed
 * hierarchy's polymorphic serializer to write it. Navigation is unaffected —
 * it has always resolved each concrete member's own serializer.
 *
 * Detail routes carry an encoded [org.gaindrive.android.data.model.ItemRef]
 * rather than a bare id — a bare id would be ambiguous the moment a second
 * server is configured. The display name travels alongside so the app bar has
 * something to show before the body has loaded.
 */
@Serializable
sealed interface Route {

	@Serializable
	data object Artists : Route

	/**
	 * [artistRefs] is comma-separated: a merged artist row stands for the same
	 * artist on several servers, each with its own id, and the albums screen
	 * has to ask all of them.
	 *
	 * [fromUploads] travels down from the Library tab's Uploads slice, and is
	 * the *only* way anything below knows an album is a personal upload — an
	 * album ref says which server and which id, never where in the library it
	 * sits, and the same album is reachable from search, starred and the play
	 * queue where the answer would be no. Defaulted, so every one of those call
	 * sites is unchanged and gets the safe answer.
	 *
	 * [fromCategories] is the same bargain for the Categories slice, and says
	 * the artist is a *section* — Film, Series — rather than a performer. It
	 * decides whether the header has a portrait and a biography to wait for:
	 * a section has neither by construction, since the server refuses the
	 * lookup for one. Defaulted for the same reason, so a section reached from
	 * search merely keeps the placeholder it has today.
	 */
	@Serializable
	data class Albums(
		val artistRefs: String,
		val artistName: String,
		val fromUploads: Boolean = false,
		val fromCategories: Boolean = false,
	) : Route

	/**
	 * [fromUploads] as on [Albums], which is where it is passed down from.
	 *
	 * [autoPlayRef] and [autoPlayMs] start a track as soon as the listing has
	 * loaded, and exist for one caller: a chapter match in search. A marker has
	 * no id anything can stream, so acting on one means opening the album its
	 * recording sits in and starting that recording partway through.
	 *
	 * Opening the album rather than playing the track from the search screen is
	 * not only about there being no `getSong` here. `nativeSeek` is false on
	 * every search result — only `getAlbum`, `getMusicDirectory`, `getVideos`
	 * and `getSong` select the codec columns it is computed from — so playing a
	 * hit directly would send a perfectly remuxable concert down the re-encode
	 * path every time. Reading the entry again through the album listing is
	 * what puts it on the right tier, and it gives the queue the rest of the
	 * recording's album besides.
	 *
	 * Both defaulted, so the four existing call sites are unchanged.
	 */
	@Serializable
	data class Album(
		val albumRef: String,
		val albumTitle: String,
		val fromUploads: Boolean = false,
		val autoPlayRef: String? = null,
		val autoPlayMs: Long = 0,
	) : Route

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
	data object SettingsCasting : Route

	@Serializable
	data object SettingsAppearance : Route

	/** Null [serverId] means "add a server" — the same screen serves both. */
	@Serializable
	data class ServerEdit(val serverId: String? = null) : Route

	/**
	 * Handing a URL shared with the app to a server, which fetches it into the
	 * account's own uploads.
	 *
	 * The URL travels in the route rather than in a holder somewhere, so
	 * `SavedStateHandle.toRoute` restores it after a configuration change or
	 * process death without the sharing app being involved again. It is already
	 * the extracted link, not the shared text — see
	 * [org.gaindrive.android.data.extractSharedUrl].
	 *
	 * A URL is full of characters a path segment cares about. That is safe for
	 * the same reason [Albums] is: navigation encodes a String argument on the
	 * way in, and `artistRefs` has carried an embedded `/` since it existed.
	 */
	@Serializable
	data class FetchUrl(val url: String) : Route

	/**
	 * The picture for whatever video is currently playing.
	 *
	 * Carries no argument: the surface shows what the player is playing, and an
	 * id here could disagree with that the moment the queue advances. Leaving
	 * the screen does not stop playback — the sound continues and the
	 * mini-player takes over.
	 */
	@Serializable
	data object Video : Route
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
