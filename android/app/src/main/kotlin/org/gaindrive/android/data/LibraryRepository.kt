package org.gaindrive.android.data

import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.async
import kotlinx.coroutines.awaitAll
import kotlinx.coroutines.coroutineScope
import kotlinx.coroutines.flow.Flow
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.asStateFlow
import kotlinx.coroutines.flow.channelFlow
import kotlinx.coroutines.flow.first
import kotlinx.coroutines.flow.update
import kotlinx.coroutines.launch
import kotlinx.coroutines.sync.Mutex
import kotlinx.coroutines.sync.withLock
import kotlinx.coroutines.withContext
import org.gaindrive.android.data.local.LocalLibrary
import org.gaindrive.android.data.model.Album
import org.gaindrive.android.data.model.AlbumDetail
import org.gaindrive.android.data.model.AlbumNotes
import org.gaindrive.android.data.model.Artist
import org.gaindrive.android.data.model.ArtistIndex
import org.gaindrive.android.data.model.ArtistInfo
import org.gaindrive.android.data.model.BrowseScope
import org.gaindrive.android.data.model.ItemRef
import org.gaindrive.android.data.model.LibrarySelection
import org.gaindrive.android.data.model.Playlist
import org.gaindrive.android.data.model.ServerConfig
import org.gaindrive.android.data.model.ServerId
import org.gaindrive.android.data.model.Song
import org.gaindrive.android.data.model.StarKind
import org.gaindrive.android.net.SubsonicClient
import org.gaindrive.android.net.SubsonicClientFactory
import org.gaindrive.android.net.requireOk
import org.gaindrive.android.net.runCatchingCancellable
import org.gaindrive.android.net.userMessage
import javax.inject.Inject
import javax.inject.Singleton

/**
 * Reads the music library, from one server or from all of them at once.
 *
 * Everything returns domain models carrying an [ItemRef], so a caller can
 * always tell which server an item came from. Queries that span servers return
 * a [MergedResult]: one server being unreachable degrades the view instead of
 * emptying it.
 */
@Singleton
class LibraryRepository @Inject constructor(
	private val registry: ServerRegistry,
	private val settings: SettingsStore,
	private val clients: SubsonicClientFactory,
	private val local: LocalLibrary,
) {

	suspend fun artistIndexes(
		scope: BrowseScope,
		personalOnly: Boolean = false,
	): MergedResult<List<ArtistIndex>> =
		fanOut(
			scope,
			// The mirror does not model the personal-only filter — it is a view
			// of the same artists — so an unreachable server contributes
			// everything stored for it rather than nothing at all.
			fallback = { config -> local.artistIndexes(config.id).takeIf { it.isNotEmpty() } },
		) { client, config ->
			// Positional: SubsonicClient reaches the API through interface
			// delegation, so named arguments lean on generated parameter names.
			client.getArtists(if (personalOnly) "true" else null)
				.requireOk().artists?.index.orEmpty()
				.map { it.toDomain(config.id) }
				// Buckets with no artists are noise in a sticky-header list.
				.filter { it.artists.isNotEmpty() }
				.also { local.saveArtistIndexes(config.id, it) }
		}.map { mergeArtistIndexes(it) }

	/**
	 * The union of one artist's albums across every server that has them.
	 *
	 * Takes a list because a merged artist row stands for several servers'
	 * artists at once. Albums are not deduplicated — the same album on two
	 * servers is two rows, each badged.
	 */
	suspend fun albumsOfArtist(refs: List<ItemRef>): MergedResult<List<Album>> {
		// Read once per query rather than once per row, and before the fan-out
		// so the transform below stays non-suspending.
		val collapse = collapseDuplicates()
		return fanOutRefs(
			refs,
			fallback = { ref -> local.albumsOfArtist(ref).takeIf { it.isNotEmpty() } },
		) { client, ref ->
			client.getArtist(ref.id).requireOk().artist?.album.orEmpty()
				.map { it.toDomain(ref.server) }
				// Stored before merging: the merge collapses the same album on
				// two servers into one row, and the mirror has to keep both.
				.also { local.saveAlbums(ref.server, it) }
		}.map { perServer ->
			val all = perServer.flatten()
			if (collapse) mergeAlbums(all) else all
		}
	}

	suspend fun artist(ref: ItemRef): Artist? = networkFirst({ local.artist(ref) }) {
		onServer(ref.server) { client ->
			client.getArtist(ref.id).requireOk().artist?.toDomain(ref.server)
		}?.also { local.saveArtist(ref.server, it) }
	}

	/**
	 * Biography and links for an artist.
	 *
	 * The server may go out to MusicBrainz and Wikipedia to answer this the
	 * first time, so it can be slow or fail outright. Callers must fetch it
	 * separately from the album list and never let it hold that list up.
	 */
	suspend fun artistInfo(ref: ItemRef): ArtistInfo? =
		onServer(ref.server) { client ->
			client.getArtistInfo2(ref.id).requireOk().artistInfo2?.toDomain()
				?.takeIf { !it.isEmpty }
		}

	/**
	 * Album with its tracks — one request, so the detail screen has something
	 * to show as soon as possible. The notes live in [albumNotes] and must be
	 * fetched separately: a lookup that may never succeed cannot be allowed to
	 * cost the user their track list.
	 */
	suspend fun albumDetail(album: ItemRef): AlbumDetail? =
		networkFirst({ local.albumDetail(album) }) {
			onServer(album.server) { client ->
				val dto = client.getAlbum(album.id).requireOk().album ?: return@onServer null
				AlbumDetail(
					album = dto.toDomain(album.server),
					songs = dto.song.map { it.toDomain(album.server) },
				)
			}?.also { local.saveAlbumDetail(album.server, it) }
		}

	suspend fun albumNotes(album: ItemRef): AlbumNotes? =
		onServer(album.server) { client ->
			client.getAlbumInfo2(album.id).requireOk().albumInfo2?.toDomain()
		}

	/**
	 * Bumped whenever a playlist is created, changed or deleted. The playlists
	 * screen watches it, so a playlist made from a track's action sheet three
	 * screens away is there when the user arrives rather than waiting for a
	 * pull to refresh.
	 */
	private val _playlistRevision = MutableStateFlow(0)
	val playlistRevision: StateFlow<Int> = _playlistRevision.asStateFlow()

	/** Grouped by server: a playlist belongs to one and cannot be merged. */
	suspend fun playlists(scope: BrowseScope): MergedResult<List<ServerSection<Playlist>>> =
		fanOutSections(
			scope,
			fallback = { config -> local.playlists(config.id).takeIf { it.isNotEmpty() } },
		) { client, config -> playlistsFrom(client, config.id) }

	/**
	 * One server's playlists, for the "add to playlist" picker — it can only
	 * offer the playlists of the server that owns the track.
	 */
	suspend fun playlistsOf(server: ServerId): List<Playlist> =
		onServer(server) { client -> playlistsFrom(client, server) }

	private suspend fun playlistsFrom(client: SubsonicClient, server: ServerId): List<Playlist> =
		client.getPlaylists().requireOk().playlists?.playlist.orEmpty()
			.map { it.toDomain(server) }
			.also { local.savePlaylists(server, it) }

	suspend fun playlist(ref: ItemRef): Playlist? = networkFirst({ local.playlist(ref) }) {
		onServer(ref.server) { client ->
			client.getPlaylist(ref.id).requireOk().playlist?.toDomain(ref.server)
		}?.also { local.savePlaylist(ref.server, it) }
	}

	/**
	 * One server's results. Search fans out through [searchProgressively],
	 * which needs each server's answer on its own to emit as it arrives.
	 */
	private suspend fun search(
		server: ServerId,
		query: String,
		artistCount: Int,
		albumCount: Int,
		songCount: Int,
	): LibrarySelection = onServer(server) { client ->
		val found = client.search3(query, artistCount, albumCount, songCount)
			.requireOk().searchResult3?.toDomain(server) ?: LibrarySelection()
		local.saveSelection(server, found)
		found
	}

	/**
	 * Emits a cumulative result as each server answers rather than waiting for
	 * the slowest — with several servers configured, the fastest usually has
	 * what the user was looking for.
	 *
	 * Every emission is rebuilt in registry order from what has arrived so far,
	 * so a slow server filling in later inserts its rows in place instead of
	 * appending them and shuffling the list under the user's finger.
	 */
	fun searchProgressively(
		scope: BrowseScope,
		query: String,
		artistCount: Int,
		albumCount: Int,
		songCount: Int,
	): Flow<MergedResult<LibrarySelection>> = channelFlow {
		val servers = serversIn(scope)
		if (servers.isEmpty()) {
			send(MergedResult(LibrarySelection()))
			return@channelFlow
		}
		val collapse = collapseDuplicates()

		val answers = arrayOfNulls<LibrarySelection>(servers.size)
		val failures = arrayOfNulls<ServerFailure>(servers.size)
		val guard = Mutex()

		servers.forEachIndexed { index, config ->
			launch {
				val result = runCatchingCancellable {
					search(config.id, query, artistCount, albumCount, songCount)
				}
				// Resolved before taking the lock: this reads a database, and
				// holding up the servers that did answer to do it would defeat
				// the point of emitting progressively.
				val fallback = result.exceptionOrNull()?.let {
					local.search(config.id, query, artistCount, albumCount, songCount)
						.takeIf { found -> !found.isEmpty }
				}
				guard.withLock {
					result.fold(
						onSuccess = { answers[index] = it },
						onFailure = {
							failures[index] = config.failure(it, storedShown = fallback != null)
							if (fallback != null) answers[index] = fallback
						},
					)
					val arrived = answers.filterNotNull()
					send(
						MergedResult(
							items = LibrarySelection(
								// Merged by name like the artist list; albums
								// and songs are concatenated, never merged.
								artists = mergeArtists(arrived.map { it.artists }),
								albums = arrived.flatMap { it.albums }
									.let { if (collapse) mergeAlbums(it) else it },
								songs = arrived.flatMap { it.songs },
							),
							failures = failures.filterNotNull(),
						)
					)
				}
			}
		}
	}

	/**
	 * Still single-server: nothing shows starred content yet, and starred items
	 * are per-account server-side state, so the screen that eventually does
	 * will want [ServerSection]s rather than one merged list.
	 */
	suspend fun starred(server: ServerId): LibrarySelection =
		onServer(server) { client ->
			client.getStarred2().requireOk().starred2?.toDomain(server) ?: LibrarySelection()
		}

	/**
	 * Recently played, grouped by server. A gaindrive extension, so a server
	 * that does not implement it contributes an empty section rather than a
	 * failure — the feature being absent is not the server being down.
	 */
	suspend fun recentSongs(scope: BrowseScope, size: Int = 50): MergedResult<List<ServerSection<Song>>> =
		fanOutSections(scope) { client, config ->
			runCatchingCancellable {
				client.getRecentSongs(size).requireOk().recentSongs?.song.orEmpty()
					.map { it.toDomain(config.id) }
			}.getOrDefault(emptyList())
		}

	/**
	 * Reports a play. Failures are swallowed: a lost scrobble costs a play
	 * count, whereas letting it propagate would interrupt playback — a bad
	 * trade in a music player.
	 */
	suspend fun scrobble(ref: ItemRef, submission: Boolean) {
		runCatchingCancellable {
			onServer(ref.server) { client -> client.scrobble(ref.id, submission).requireOk() }
		}
	}

	suspend fun setStarred(ref: ItemRef, kind: StarKind, starred: Boolean) {
		onServer(ref.server) { client ->
			val song = ref.id.takeIf { kind == StarKind.SONG }
			val album = ref.id.takeIf { kind == StarKind.ALBUM }
			val artist = ref.id.takeIf { kind == StarKind.ARTIST }
			if (starred) client.star(song, album, artist).requireOk()
			else client.unstar(song, album, artist).requireOk()
		}
	}

	suspend fun createPlaylist(server: ServerId, name: String, songIds: List<String>) {
		onServer(server) { client -> client.createPlaylist(name, songIds).requireOk() }
		bumpPlaylists()
	}

	suspend fun addToPlaylist(playlist: ItemRef, songId: String) {
		onServer(playlist.server) { client ->
			client.updatePlaylist(playlist.id, listOf(songId), emptyList()).requireOk()
		}
		bumpPlaylists()
	}

	/** Removes by position; the caller must not let indices shift under it. */
	suspend fun removeFromPlaylist(playlist: ItemRef, index: Int) {
		onServer(playlist.server) { client ->
			client.updatePlaylist(playlist.id, emptyList(), listOf(index)).requireOk()
		}
		bumpPlaylists()
	}

	suspend fun deletePlaylist(playlist: ItemRef) {
		onServer(playlist.server) { client -> client.deletePlaylist(playlist.id).requireOk() }
		bumpPlaylists()
	}

	/** Only after the call succeeds: a failed edit changed nothing. */
	private fun bumpPlaylists() = _playlistRevision.update { it + 1 }

	/** The configured servers, in registry order — which is the merge tie-break. */
	suspend fun enabledServers(): List<ServerConfig> = registry.enabledServers.first()

	/**
	 * A URL builder covering every enabled server. Resolve once per screen
	 * load and reuse for the whole list — see [CoverUrls] on why not per item.
	 */
	suspend fun coverUrls(): CoverUrls = withContext(Dispatchers.IO) {
		CoverUrls(enabledServers().associate { it.id to clients.clientFor(it) })
	}

	/**
	 * Resolves the client for a server and runs [block] off the main thread.
	 * Throws if the server is not configured, which is a programming error
	 * rather than a condition to handle: refs are only produced from servers
	 * that exist.
	 */
	private suspend fun <T> onServer(
		server: ServerId,
		block: suspend (SubsonicClient) -> T,
	): T = withContext(Dispatchers.IO) {
		val config = registry.get(server)
			?: error("No such server configured: $server")
		block(clients.clientFor(config))
	}

	/** The user's answer to "is the same album on two servers one row or two?" */
	private suspend fun collapseDuplicates(): Boolean =
		settings.mergeDuplicateAlbums.first()

	/** The servers [scope] covers, in registry order. */
	private suspend fun serversIn(scope: BrowseScope): List<ServerConfig> {
		val enabled = registry.enabledServers.first()
		return when (scope) {
			is BrowseScope.AllServers -> enabled
			is BrowseScope.OneServer -> enabled.filter { it.id == scope.id }
		}
	}

	private suspend fun <T> fanOut(
		scope: BrowseScope,
		fallback: (suspend (ServerConfig) -> T?)? = null,
		block: suspend (SubsonicClient, ServerConfig) -> T,
	): MergedResult<List<T>> {
		val gathered = gather(serversIn(scope), fallback, block)
		return MergedResult(gathered.answers.map { it.second }, gathered.failures)
	}

	/** As [fanOut], but keeping each server's items in their own section. */
	private suspend fun <T> fanOutSections(
		scope: BrowseScope,
		fallback: (suspend (ServerConfig) -> List<T>?)? = null,
		block: suspend (SubsonicClient, ServerConfig) -> List<T>,
	): MergedResult<List<ServerSection<T>>> {
		val gathered = gather(serversIn(scope), fallback, block)
		return MergedResult(
			// A heading over nothing is noise; a server with no playlists just
			// does not appear.
			items = gathered.answers.map { (config, items) -> ServerSection(config, items) }
				.filter { it.items.isNotEmpty() },
			failures = gathered.failures,
		)
	}

	/**
	 * The same fan-out driven by refs rather than by scope, for a merged row
	 * that names a different id on each of its servers.
	 */
	private suspend fun <T> fanOutRefs(
		refs: List<ItemRef>,
		fallback: (suspend (ItemRef) -> T?)? = null,
		block: suspend (SubsonicClient, ItemRef) -> T,
	): MergedResult<List<T>> = coroutineScope {
		val configs = registry.enabledServers.first()
		val answers = refs.mapNotNull { ref ->
			// A ref whose server has since been removed is skipped rather than
			// reported: it is stale navigation state, not a server that failed.
			val config = configs.firstOrNull { it.id == ref.server } ?: return@mapNotNull null
			async(Dispatchers.IO) {
				Triple(config, ref, runCatchingCancellable { block(clients.clientFor(config), ref) })
			}
		}.awaitAll()

		val items = mutableListOf<T>()
		val failures = mutableListOf<ServerFailure>()
		answers.forEach { (config, ref, result) ->
			result.fold(
				onSuccess = { items += it },
				onFailure = { error ->
					val stored = fallback?.invoke(ref)
					stored?.let { items += it }
					failures += config.failure(error, storedShown = stored != null)
				},
			)
		}
		MergedResult(items, failures)
	}

	/**
	 * Queries every server at once, and falls back to what was stored for any
	 * that did not answer.
	 *
	 * The failure is recorded either way. A stored answer is not the same as a
	 * live one, and a screen that quietly showed yesterday's library as though
	 * it were current would be worse than one that admits a server is down.
	 *
	 * In parallel, not in sequence: three servers queried one after another
	 * would make every browse screen as slow as the sum of them, and one that
	 * has gone away would hold up the two that are fine until it times out.
	 */
	private suspend fun <T> gather(
		servers: List<ServerConfig>,
		fallback: (suspend (ServerConfig) -> T?)?,
		block: suspend (SubsonicClient, ServerConfig) -> T,
	): Gathered<T> = coroutineScope {
		val answers = servers.map { config ->
			async(Dispatchers.IO) {
				config to runCatchingCancellable { block(clients.clientFor(config), config) }
			}
		}.awaitAll()

		val items = mutableListOf<Pair<ServerConfig, T>>()
		val failures = mutableListOf<ServerFailure>()
		answers.forEach { (config, result) ->
			result.fold(
				onSuccess = { items += config to it },
				onFailure = { error ->
					val stored = fallback?.invoke(config)
					stored?.let { items += config to it }
					failures += config.failure(error, storedShown = stored != null)
				},
			)
		}
		Gathered(items, failures)
	}

	private data class Gathered<T>(
		val answers: List<Pair<ServerConfig, T>>,
		val failures: List<ServerFailure>,
	)

	/**
	 * The server first, the mirror only if it fails — and the original error if
	 * the mirror has nothing either.
	 *
	 * Rethrowing matters: "the server is unreachable" and "there is no such
	 * album" want different screens, and swallowing the error would turn the
	 * first into the second.
	 */
	private suspend fun <T : Any> networkFirst(
		stored: suspend () -> T?,
		block: suspend () -> T?,
	): T? = runCatchingCancellable { block() }
		.getOrElse { error -> stored() ?: throw error }

	/**
	 * [storedShown] changes the wording rather than being suppressed by it: the
	 * server really did fail, and the rows on screen really are not current.
	 * Saying only one of those would mislead in one direction or the other.
	 */
	private fun ServerConfig.failure(cause: Throwable, storedShown: Boolean = false) =
		ServerFailure(
			id,
			name,
			if (storedShown) "${cause.userMessage()} Showing what was stored."
			else cause.userMessage(),
		)
}
