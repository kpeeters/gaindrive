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
import org.gaindrive.android.data.browse.browseSource
import org.gaindrive.android.data.browse.RootRequest
import org.gaindrive.android.data.browse.listingRequests
import org.gaindrive.android.data.browse.uploadsRequest
import org.gaindrive.android.data.cache.AudioCache
import org.gaindrive.android.data.local.LocalLibrary
import org.gaindrive.android.data.local.StoredFilter
import org.gaindrive.android.data.model.Album
import org.gaindrive.android.data.model.AlbumDetail
import org.gaindrive.android.data.model.AlbumNotes
import org.gaindrive.android.data.model.Artist
import org.gaindrive.android.data.model.ArtistIndex
import org.gaindrive.android.data.model.ArtistInfo
import org.gaindrive.android.data.model.BrowseScope
import org.gaindrive.android.data.model.Chapter
import org.gaindrive.android.data.model.ItemRef
import org.gaindrive.android.data.model.LibraryListing
import org.gaindrive.android.data.model.LibrarySection
import org.gaindrive.android.data.model.LibrarySelection
import org.gaindrive.android.data.model.MusicRoot
import org.gaindrive.android.data.model.Playlist
import org.gaindrive.android.data.model.ServerConfig
import org.gaindrive.android.data.model.ServerId
import org.gaindrive.android.data.model.Song
import org.gaindrive.android.data.model.StarKind
import org.gaindrive.android.net.OfflineException
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
	private val connectivity: Connectivity,
	private val audioCache: AudioCache,
	private val musicRoots: MusicRoots,
	private val accounts: Accounts,
	private val libraryRevision: LibraryRevision,
) {

	/**
	 * Whether to skip the network entirely.
	 *
	 * Read per query rather than collected: a query already knows what it is
	 * doing at the moment it runs, and a flow here would mean every caller
	 * dealing with a value that could change under it mid-fan-out.
	 */
	private val offline: Boolean get() = !connectivity.online.value

	/**
	 * What to hide from browse listings, or null when there is nothing to hide.
	 *
	 * Null whenever the app is online, including when a server has just failed:
	 * a stored copy shown because one request timed out should still be the
	 * whole library, since the connection may come straight back. Only a
	 * genuinely offline app trims itself to what it can play.
	 */
	private suspend fun storedOnly(): StoredFilter? =
		if (offline) local.storedFilter(audioCache.cachedKeys.value) else null

	/**
	 * One server's contribution to the merged list: its category buckets and
	 * its artist buckets. A private pair with names, because a bare Pair at
	 * the merge site would leave which half is which to memory.
	 */
	private data class ServerListing(
		val categories: List<ArtistIndex>,
		val artists: List<ArtistIndex>,
	)

	/**
	 * The Library screen's merged list: every category folder from every
	 * `categories` root, and every artist - including all roots of a server
	 * that names no kinds; see `listingRequests`.
	 *
	 * A typed server is asked once per kind it has, concurrently - the halves
	 * are independent requests against the same session. A kind the server
	 * lacks is not asked for at all rather than filtered out of an answer:
	 * a server predating library roots ignores the unknown contentType
	 * parameter and answers with its entire library, so the request itself is
	 * what would put the same folders in both groups. If either half fails the
	 * whole server falls back to its mirror, which is the same all-or-nothing
	 * failure reporting one request had.
	 */
	suspend fun libraryListing(scope: BrowseScope): MergedResult<LibraryListing> {
		val stored = storedOnly()
		return fanOut(
			scope,
			fallback = { config ->
				ServerListing(
					categories = local.artistIndexes(config.id, LibrarySection.CATEGORIES)
						.let { stored?.filterIndexes(it) ?: it },
					artists = local.artistIndexes(config.id, LibrarySection.ARTISTS)
						.let { stored?.filterIndexes(it) ?: it },
				).takeIf { it.categories.isNotEmpty() || it.artists.isNotEmpty() }
			},
		) { client, config ->
			val requests = listingRequests(config.browseByFolder, rootsOf(client, config))

			suspend fun fetch(request: RootRequest?, section: LibrarySection) =
				if (request == null) {
					emptyList()
				} else {
					config.browseSource.indexes(client, config.id, request)
						// Buckets with no artists are noise in a sticky-header
						// list.
						.filter { it.artists.isNotEmpty() }
						.also { local.saveArtistIndexes(config.id, section, it) }
				}

			coroutineScope {
				val categories = async {
					fetch(requests.categories, LibrarySection.CATEGORIES)
				}
				val artists = async { fetch(requests.artists, LibrarySection.ARTISTS) }
				ServerListing(categories.await(), artists.await())
			}
		}.map { perServer ->
			LibraryListing(
				categories = mergeCategories(perServer.map { it.categories }),
				artists = mergeArtistIndexes(perServer.map { it.artists }),
			)
		}
	}

	/**
	 * The uploads listing - this account's own on every server it may upload
	 * to, or everyone's where it is the admin. A server where it may not
	 * upload contributes nothing rather than a failure: having no upload
	 * rights on one server of several is a fact, not an outage.
	 */
	suspend fun uploadIndexes(scope: BrowseScope): MergedResult<List<ArtistIndex>> {
		val stored = storedOnly()
		return fanOut(
			scope,
			fallback = { config ->
				local.artistIndexes(config.id, LibrarySection.UPLOADS)
					.let { stored?.filterIndexes(it) ?: it }
					.takeIf { it.isNotEmpty() }
			},
		) { client, config ->
			val facts = accounts.factsFor(config)
			if (!facts.canUpload) return@fanOut emptyList()
			config.browseSource.indexes(client, config.id, uploadsRequest(facts.isAdmin))
				.filter { it.artists.isNotEmpty() }
				.also { local.saveArtistIndexes(config.id, LibrarySection.UPLOADS, it) }
		}.map { mergeArtistIndexes(it) }
	}

	/**
	 * Whether the upload icon is worth drawing: some server in [scope] takes
	 * uploads from this account. Offline the accounts cache is unreachable, so
	 * the honest answer is whether the mirror holds an uploads listing at all.
	 */
	suspend fun canUpload(scope: BrowseScope): Boolean {
		if (offline) {
			return LibrarySection.UPLOADS.id in
				local.storedContentTypes(serversIn(scope).map { it.id })
		}
		return serversIn(scope).any { config ->
			runCatchingCancellable { accounts.canUploadTo(config) }.getOrDefault(false)
		}
	}

	/**
	 * Which sections the scope's roots offer, for the fetch panel's field
	 * labels. Never contains [LibrarySection.UPLOADS] - uploads is not a kind
	 * of root - and never empty: a server nothing is known about is assumed to
	 * hold artists, which is what every Subsonic server without roots is.
	 */
	suspend fun availableSections(scope: BrowseScope): List<LibrarySection> {
		val ids = if (offline) {
			local.storedContentTypes(serversIn(scope).map { it.id })
		} else {
			fanOut(scope, fallback = { null }) { client, config ->
				val requests = listingRequests(config.browseByFolder, rootsOf(client, config))
				listOfNotNull(
					LibrarySection.ARTISTS.id.takeIf { requests.artists != null },
					LibrarySection.CATEGORIES.id.takeIf { requests.categories != null },
				)
			}.items.flatten()
		}
		return LibrarySection.entries
			.filter { it != LibrarySection.UPLOADS && it.id in ids }
			.ifEmpty { listOf(LibrarySection.ARTISTS) }
	}

	/** This server's configured roots, fetched once per session. */
	private suspend fun rootsOf(client: SubsonicClient, config: ServerConfig): List<MusicRoot> =
		musicRoots.of(config.id) {
			client.getMusicFolders().requireOk()
				.musicFolders?.musicFolder.orEmpty()
				.map { it.toDomain() }
		}

	/**
	 * The union of one artist's albums across every server that has them.
	 *
	 * Takes a list because a merged artist row stands for several servers'
	 * artists at once. Albums are not deduplicated - the same album on two
	 * servers is two rows, each badged.
	 */
	suspend fun albumsOfArtist(refs: List<ItemRef>): MergedResult<List<Album>> {
		// Read once per query rather than once per row, and before the fan-out
		// so the transform below stays non-suspending.
		val collapse = collapseDuplicates()
		val stored = storedOnly()
		return fanOutRefs(
			refs,
			fallback = { ref ->
				local.albumsOfArtist(ref)
					.let { stored?.filterAlbums(it) ?: it }
					.takeIf { it.isNotEmpty() }
			},
		) { client, config, ref ->
			config.browseSource.albums(client, ref)
				// Stored before merging: the merge collapses the same album on
				// two servers into one row, and the mirror has to keep both.
				.also { local.saveAlbums(ref.server, it) }
		}.map { perServer ->
			val all = perServer.flatten()
			if (collapse) mergeAlbums(all) else all
		}
	}

	suspend fun artist(ref: ItemRef): Artist? = networkFirst({ local.artist(ref) }) {
		withServer(ref.server) { client, config ->
			config.browseSource.artist(client, ref)
		}?.also { local.saveArtist(ref.server, it) }
	}

	/**
	 * Biography and links for an artist.
	 *
	 * The server may go out to MusicBrainz and Wikipedia to answer this the
	 * first time, so it can be slow or fail outright. Callers must fetch it
	 * separately from the album list and never let it hold that list up.
	 *
	 * Mirrored, so an artist read once online still has a biography offline.
	 * Network-first, which is also what makes pull-to-refresh mean "ask the
	 * server again": the stored copy is only consulted when the server cannot
	 * be reached. Nothing here asks the server to redo its own provider
	 * lookup, which is a server-side action and stays in the web client.
	 */
	suspend fun artistInfo(ref: ItemRef): ArtistInfo? =
		storedOrNull({ local.artistInfo(ref) }) {
			val fetched = onServer(ref.server) { client ->
				client.getArtistInfo2(ref.id).requireOk().artistInfo2?.toDomain()
			}
			// An all-blank answer is collapsed before it is stored, so an
			// artist the providers know nothing about does not get a row
			// asserting that they know nothing about them.
			fetched?.takeIf { !it.isEmpty }?.also { local.saveArtistInfo(ref, it) }
		}

	/**
	 * Album with its tracks - one request, so the detail screen has something
	 * to show as soon as possible. The notes live in [albumNotes] and must be
	 * fetched separately: a lookup that may never succeed cannot be allowed to
	 * cost the user their track list.
	 *
	 * "One request" holds for the tag hierarchy and for the common folder case.
	 * An album whose discs are subfolders costs one more round - see
	 * `FolderSource`, which is where that is arranged rather than here.
	 */
	suspend fun albumDetail(album: ItemRef): AlbumDetail? =
		networkFirst({ local.albumDetail(album) }) {
			withServer(album.server) { client, config ->
				config.browseSource.albumDetail(client, album)
			}?.also { local.saveAlbumDetail(album.server, it) }
		}

	/** Mirrored and network-first; see [artistInfo]. */
	suspend fun albumNotes(album: ItemRef): AlbumNotes? =
		storedOrNull({ local.albumNotes(album) }) {
			val fetched = onServer(album.server) { client ->
				client.getAlbumInfo2(album.id).requireOk().albumInfo2?.toDomain()
			}
			// `AlbumDetailViewModel` checks this too, which is where the
			// check used to live alone. It has to happen here as well: an
			// all-blank answer stored is a blank row read back for ever.
			fetched?.takeIf { !it.isEmpty }?.also { local.saveAlbumNotes(album, it) }
		}

	/**
	 * The chapter markers of every chaptered item in one album folder, keyed by
	 * the item they belong to.
	 *
	 * Read from the scan's index rather than from each file, which is what
	 * makes it affordable on a browse path: `getChapters` per item would be a
	 * file read - or an `ffprobe` for a video without a sidecar - every time
	 * somebody opens an album. The playback path uses the file instead; see
	 * [ChapterTracks].
	 *
	 * Mirrored, keyed on the song and the marker's own position, which is the
	 * shape `playlist_songs` already uses for a child row that has no id of its
	 * own. The earlier argument that a marker is unstorable was about a chapter
	 * as an addressable item, which it still is not.
	 *
	 * A failed request falls back to the stored markers rather than
	 * propagating: the caller already treats an empty map as "this album has
	 * none", so an error and an absence were indistinguishable to it anyway.
	 */
	suspend fun albumChapters(album: ItemRef): Map<ItemRef, List<Chapter>> {
		if (offline) return local.chaptersOfAlbum(album)
		val found = runCatchingCancellable {
			onServer(album.server) { client ->
				client.getAlbumChapters(album.id).requireOk().albumChapters?.song.orEmpty()
					.map { it.toDomain(album.server) }
			}
		}.getOrElse { return local.chaptersOfAlbum(album) }
		// An entry with no markers cannot happen - the server omits those - but
		// dropping one here is what lets every caller treat "in the map" and
		// "has chapters" as the same question.
		val bySong = found.filter { it.chapters.isNotEmpty() }
			.associate { it.ref to it.chapters }
		// Stored even when empty: that is what clears markers the server has
		// since lost, and an album with none is the overwhelmingly common case.
		local.saveAlbumChapters(album, bySong)
		return bySong
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
	suspend fun playlists(scope: BrowseScope): MergedResult<List<ServerSection<Playlist>>> {
		val stored = storedOnly()
		return fanOutSections(
			scope,
			fallback = { config ->
				local.playlists(config.id)
					.let { stored?.filterPlaylists(it) ?: it }
					.takeIf { it.isNotEmpty() }
			},
		) { client, config -> playlistsFrom(client, config.id) }
	}

	/**
	 * One server's playlists, for the "add to playlist" picker - it can only
	 * offer the playlists of the server that owns the track.
	 */
	suspend fun playlistsOf(server: ServerId): List<Playlist> {
		// Not backed by the mirror the way [playlists] is: this picker exists to
		// add a track, and adding is a write that offline cannot do. Failing
		// here says so once instead of after the user has picked one.
		requireOnline()
		return onServer(server) { client -> playlistsFrom(client, server) }
	}

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
		chapterCount: Int,
	): LibrarySelection = withServer(server) { client, config ->
		// Which search endpoint is asked follows the browse mode, because a
		// result is only useful if the id it carries is one the rest of the mode
		// can open - see BrowseSource.search.
		val found = config.browseSource
			.search(client, server, query, artistCount, albumCount, songCount, chapterCount)
		// Any chapter hits ride along in `found` and are dropped here: the
		// mirror stores nothing for them, a marker having no id to key a row on.
		local.saveSelection(server, found)
		found
	}

	/**
	 * Emits a cumulative result as each server answers rather than waiting for
	 * the slowest - with several servers configured, the fastest usually has
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
		chapterCount: Int,
	): Flow<MergedResult<LibrarySelection>> = channelFlow {
		val servers = serversIn(scope)
		if (servers.isEmpty()) {
			send(MergedResult(LibrarySelection()))
			return@channelFlow
		}
		val collapse = collapseDuplicates()
		val stored = storedOnly()

		val answers = arrayOfNulls<LibrarySelection>(servers.size)
		val failures = arrayOfNulls<ServerFailure>(servers.size)
		val guard = Mutex()

		servers.forEachIndexed { index, config ->
			launch {
				// Offline: the stored index is the only index there is, and it
				// answers fast enough that emitting progressively is moot.
				val result = if (offline) {
					Result.success(
						local.search(config.id, query, artistCount, albumCount, songCount)
							.let { stored?.filterSelection(it) ?: it }
					)
				} else {
					runCatchingCancellable {
						search(config.id, query, artistCount, albumCount, songCount, chapterCount)
					}
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
								// Concatenated in registry order like songs and
								// never merged: a marker has no id to dedupe on,
								// and two servers holding the same concert hold
								// two different files either way.
								chapters = arrived.flatMap { it.chapters },
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
	 * failure - the feature being absent is not the server being down.
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
	 * count, whereas letting it propagate would interrupt playback - a bad
	 * trade in a music player.
	 */
	suspend fun scrobble(ref: ItemRef, submission: Boolean) {
		if (offline) return
		runCatchingCancellable {
			onServer(ref.server) { client -> client.scrobble(ref.id, submission).requireOk() }
		}
	}

	suspend fun setStarred(ref: ItemRef, kind: StarKind, starred: Boolean) {
		requireOnline()
		onServer(ref.server) { client ->
			val song = ref.id.takeIf { kind == StarKind.SONG }
			val album = ref.id.takeIf { kind == StarKind.ALBUM }
			val artist = ref.id.takeIf { kind == StarKind.ARTIST }
			if (starred) client.star(song, album, artist).requireOk()
			else client.unstar(song, album, artist).requireOk()
		}
	}

	/**
	 * Moves an album out of the account's uploads into the shared library.
	 *
	 * Admin only, and the server is the one that enforces that - asking here as
	 * well would be a second copy of a rule that can change under us. A refusal
	 * arrives as a [org.gaindrive.android.net.SubsonicException] for the caller
	 * to show.
	 *
	 * Two things have to happen after it succeeds, and neither is optional. The
	 * mirror of that server is dropped, because the move happened on its disk
	 * and every stored id and index bucket beneath the album now names something
	 * that is not there. And [LibraryRevision] is bumped, because the two
	 * listings that changed - the uploads slice it left and the library slice it
	 * joined - are both screens the user is *not* looking at, so nothing else
	 * would ever correct them.
	 */
	/**
	 * Whether this account administers [server], which is what [promoteAlbum]
	 * needs. False for a server that is not configured or did not answer - a
	 * permission guessed present would be an action that fails when used.
	 */
	suspend fun isAdminOn(server: ServerId): Boolean {
		val config = registry.get(server) ?: return false
		return accounts.isAdminOn(config)
	}

	/**
	 * The roots of one server that an upload could be promoted into.
	 *
	 * `getMusicFolders` already excludes the uploads root, so this is exactly
	 * the set of valid destinations and needs no filtering of its own.
	 */
	suspend fun destinationRoots(server: ServerId): List<MusicRoot> =
		withServer(server) { client, config -> rootsOf(client, config) }

	/**
	 * The existing level-1 folders of one root - artists under an `artists`
	 * root, categories under a `categories` one - for the promote picker's
	 * suggestions.
	 *
	 * Asked by `musicFolderId` rather than by content type, because a
	 * destination is one specific root and a kind may span several. Not read
	 * from the mirror: that is keyed by slice, which is the coarser thing, and a
	 * promote needs a network anyway.
	 */
	suspend fun foldersIn(server: ServerId, musicFolderId: String): List<String> =
		onServer(server) { client ->
			client.getArtists(null, null, musicFolderId)
				.requireOk().artists?.index.orEmpty()
				.flatMap { index -> index.artist.map { it.name } }
		}

	/**
	 * Moves an album out of the account's uploads into the shared library.
	 *
	 * Admin only, and the server is the one that enforces that - asking here as
	 * well would be a second copy of a rule that can change under us. A refusal
	 * arrives as a [org.gaindrive.android.net.SubsonicException] for the caller
	 * to show.
	 *
	 * [musicFolderId] and [folder] say where it lands: a root, and one level
	 * under it. Both are required by the server, and non-null here to say so -
	 * they were briefly optional and each default was a guess that filed things
	 * wrongly, the root one unable to reach a `categories` root at all.
	 *
	 * The endpoint behind it is the general `moveAlbum`, which also renames in
	 * place and re-files under another artist. The name kept here is the one
	 * thing this app does with it, and is what the screen offers.
	 *
	 * Two things have to happen after it succeeds, and neither is optional. The
	 * mirror of that server is dropped, because the move happened on its disk
	 * and every stored id and index bucket beneath the album now names something
	 * that is not there. And [LibraryRevision] is bumped, because the two
	 * listings that changed - the uploads slice it left and the library slice it
	 * joined - are both screens the user is *not* looking at, so nothing else
	 * would ever correct them.
	 */
	suspend fun promoteAlbum(album: ItemRef, musicFolderId: String, folder: String) {
		requireOnline()
		onServer(album.server) { client ->
			client.moveAlbum(album.id, musicFolderId, folder.trim()).requireOk()
		}
		local.forgetLibrary(album.server)
		libraryRevision.bump()
	}

	/**
	 * Removes one of the account's own uploaded albums from the server.
	 *
	 * Irreversible - the files go from the server's disk - and the server is
	 * what enforces that this is the caller's own upload rather than anything
	 * else with a folder id. A refusal arrives as a
	 * [org.gaindrive.android.net.SubsonicException] for the caller to show.
	 *
	 * The aftermath is a promote's: drop that server's mirror, because the
	 * album's rows and every id beneath it name something that is no longer
	 * there, and bump [LibraryRevision], because the listing that changed is one
	 * the user is not looking at.
	 */
	suspend fun deleteUpload(album: ItemRef) {
		requireOnline()
		onServer(album.server) { client -> client.deleteUpload(album.id).requireOk() }
		local.forgetLibrary(album.server)
		libraryRevision.bump()
	}

	suspend fun createPlaylist(server: ServerId, name: String, songIds: List<String>) {
		requireOnline()
		onServer(server) { client -> client.createPlaylist(name, songIds).requireOk() }
		bumpPlaylists()
	}

	suspend fun addToPlaylist(playlist: ItemRef, songId: String) {
		requireOnline()
		onServer(playlist.server) { client ->
			client.updatePlaylist(playlist.id, listOf(songId), emptyList()).requireOk()
		}
		bumpPlaylists()
	}

	/** Removes by position; the caller must not let indices shift under it. */
	suspend fun removeFromPlaylist(playlist: ItemRef, index: Int) {
		requireOnline()
		onServer(playlist.server) { client ->
			client.updatePlaylist(playlist.id, emptyList(), listOf(index)).requireOk()
		}
		bumpPlaylists()
	}

	suspend fun deletePlaylist(playlist: ItemRef) {
		requireOnline()
		onServer(playlist.server) { client -> client.deletePlaylist(playlist.id).requireOk() }
		bumpPlaylists()
	}

	/** Only after the call succeeds: a failed edit changed nothing. */
	private fun bumpPlaylists() = _playlistRevision.update { it + 1 }

	/** The configured servers, in registry order - which is the merge tie-break. */
	suspend fun enabledServers(): List<ServerConfig> = registry.enabledServers.first()

	/**
	 * A URL builder covering every enabled server. Resolve once per screen
	 * load and reuse for the whole list - see [CoverUrls] on why not per item.
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
	): T = withServer(server) { client, _ -> block(client) }

	/**
	 * As [onServer], but handing the block the configuration too - which the
	 * hierarchy queries need in order to know which of the two ways of reading
	 * it this server is set to. A sibling rather than a wider [onServer]: most
	 * of its callers name an endpoint that works the same either way, and would
	 * only have gained an ignored parameter.
	 */
	private suspend fun <T> withServer(
		server: ServerId,
		block: suspend (SubsonicClient, ServerConfig) -> T,
	): T = withContext(Dispatchers.IO) {
		val config = registry.get(server)
			?: error("No such server configured: $server")
		block(clients.clientFor(config), config)
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
		block: suspend (SubsonicClient, ServerConfig, ItemRef) -> T,
	): MergedResult<List<T>> = coroutineScope {
		val configs = registry.enabledServers.first()

		if (offline) {
			return@coroutineScope MergedResult(
				refs.filter { ref -> configs.any { it.id == ref.server } }
					.mapNotNull { fallback?.invoke(it) }
			)
		}

		val answers = refs.mapNotNull { ref ->
			// A ref whose server has since been removed is skipped rather than
			// reported: it is stale navigation state, not a server that failed.
			val config = configs.firstOrNull { it.id == ref.server } ?: return@mapNotNull null
			async(Dispatchers.IO) {
				Triple(
					config,
					ref,
					runCatchingCancellable { block(clients.clientFor(config), config, ref) },
				)
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
		// Offline: no request, and no failure note either. A per-server
		// "unreachable" banner on every screen would be noise when being
		// offline is the state the user asked for - the one banner above the
		// player has already said it.
		if (offline) {
			return@coroutineScope Gathered(
				answers = servers.mapNotNull { config ->
					fallback?.invoke(config)?.let { config to it }
				},
				failures = emptyList(),
			)
		}

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
	 * The server first, the mirror only if it fails - and the original error if
	 * the mirror has nothing either.
	 *
	 * Rethrowing matters: "the server is unreachable" and "there is no such
	 * album" want different screens, and swallowing the error would turn the
	 * first into the second.
	 */
	private suspend fun <T : Any> networkFirst(
		stored: suspend () -> T?,
		block: suspend () -> T?,
	): T? {
		if (offline) {
			// Not null: "nothing stored" and "no such album" are different
			// answers, and only the first one is worth an explanation.
			return stored() ?: throw OfflineException(
				"You are offline, and this is not stored on the device."
			)
		}
		return runCatchingCancellable { block() }
			.getOrElse { error -> stored() ?: throw error }
	}

	/**
	 * [networkFirst] for things whose absence is ordinary.
	 *
	 * A biography, album notes and chapter markers are all missing for most
	 * items, and every caller already renders that as "no section". Throwing
	 * `OfflineException` when nothing is stored, as [networkFirst] does, would
	 * dress up the normal case as a failure; returning the stored copy or null
	 * says exactly what happened.
	 *
	 * A server that answers "none" is believed and *not* fallen back on, so a
	 * biography really withdrawn stops being shown online. The stored row
	 * outlives it and is still served offline, which is the safer of the two
	 * wrong answers: offline the choice is between stale prose and none.
	 */
	private suspend fun <T : Any> storedOrNull(
		stored: suspend () -> T?,
		block: suspend () -> T?,
	): T? {
		if (offline) return stored()
		return runCatchingCancellable { block() }.getOrElse { stored() }
	}

	/** Guards a write. Nothing can be queued, so failing at once is the honest answer. */
	private fun requireOnline() {
		if (offline) throw OfflineException("You are offline, so this cannot be saved.")
	}

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
