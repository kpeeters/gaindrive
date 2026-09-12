package org.gaindrive.android.data.local

import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.asStateFlow
import kotlinx.coroutines.flow.update
import kotlinx.coroutines.withContext
import org.gaindrive.android.data.model.Album
import org.gaindrive.android.data.model.AlbumDetail
import org.gaindrive.android.data.model.Artist
import org.gaindrive.android.data.model.ArtistIndex
import org.gaindrive.android.data.model.ItemRef
import org.gaindrive.android.data.model.LibrarySection
import org.gaindrive.android.data.model.LibrarySelection
import org.gaindrive.android.data.model.Playlist
import org.gaindrive.android.data.model.ServerId
import org.gaindrive.android.data.model.Song
import javax.inject.Inject
import javax.inject.Singleton

/**
 * The mirrored library, spoken in domain models.
 *
 * Every method is scoped to a single server and returns unmerged results: the
 * cross-server merge rules live in the repository, and having a second copy of
 * them here is how the stored view and the live view would start to disagree.
 *
 * Reads return empty rather than null when a server has simply not been browsed
 * yet — "nothing stored" is not an error, it is the state everyone starts in.
 */
@Singleton
class LocalLibrary @Inject constructor(
	private val dao: LibraryDao,
) {

	private val _revision = MutableStateFlow(0)

	/**
	 * Bumped whenever anything is stored.
	 *
	 * Pins watch this: what an album or playlist pin covers is decided by the
	 * stored track lists, so a browse that changes them changes what has to be
	 * protected, without any pin having moved.
	 */
	val revision: StateFlow<Int> = _revision.asStateFlow()

	// ── Artists ─────────────────────────────────────────────────────────────

	suspend fun saveArtistIndexes(
		server: ServerId,
		section: LibrarySection,
		indexes: List<ArtistIndex>,
	) = write {
		dao.upsertArtists(
			indexes.flatMap { index ->
				index.artists.map { it.toEntity(server, index.label, section.id) }
			}
		)
	}

	suspend fun artistIndexes(server: ServerId, section: LibrarySection): List<ArtistIndex> = io {
		dao.artists(server.value, section.id)
			.groupBy { it.indexLabel }
			.map { (label, rows) -> ArtistIndex(label, rows.map { it.toDomain() }) }
	}

	/**
	 * Sections present in the mirror. Offline this is what says whether there
	 * is an uploads listing to offer, and which kinds the fetch panel's field
	 * labels can name.
	 */
	suspend fun storedContentTypes(servers: List<ServerId>): List<String> =
		io { dao.storedContentTypes(servers.map { it.value }) }

	suspend fun artist(ref: ItemRef): Artist? = io {
		dao.artist(ref.server.value, ref.id)?.toDomain()
	}

	/**
	 * One artist, outside the index-bucket context of `getArtists`. The bucket
	 * is derived from the name; if the list is fetched later the server's own
	 * answer overwrites it.
	 */
	suspend fun saveArtist(server: ServerId, artist: Artist) = write {
		// Preserve the stored kind. @Upsert replaces the whole row, and this
		// path has no idea which root the artist came from — overwriting it
		// with the default would move a category section into the Artists
		// list the next time the app is offline.
		val kind = dao.artist(server.value, artist.ref.id)?.contentType
			?: LibrarySection.ARTISTS.id
		dao.upsertArtists(listOf(artist.toEntity(server, indexLabelFor(artist.name), kind)))
	}

	// ── Albums and songs ────────────────────────────────────────────────────

	suspend fun saveAlbums(server: ServerId, albums: List<Album>) = write {
		dao.upsertAlbums(albums.map { it.toEntity(server) })
	}

	suspend fun albumsOfArtist(ref: ItemRef): List<Album> = io {
		dao.albumsOfArtist(ref.server.value, ref.id).map { it.toDomain() }
	}

	suspend fun saveAlbumDetail(server: ServerId, detail: AlbumDetail) = write {
		dao.upsertAlbums(listOf(detail.album.toEntity(server)))
		dao.upsertSongs(detail.songs.map { it.toEntity(server) })
	}

	suspend fun album(ref: ItemRef): Album? = io {
		dao.album(ref.server.value, ref.id)?.toDomain()
	}

	suspend fun albumDetail(ref: ItemRef): AlbumDetail? = io {
		val album = dao.album(ref.server.value, ref.id) ?: return@io null
		AlbumDetail(
			album = album.toDomain(),
			songs = dao.songsOfAlbum(ref.server.value, ref.id).map { it.toDomain() },
		)
	}

	suspend fun saveSongs(server: ServerId, songs: List<Song>) = write {
		dao.upsertSongs(songs.map { it.toEntity(server) })
	}

	suspend fun songsOfAlbum(ref: ItemRef): List<Song> = io {
		dao.songsOfAlbum(ref.server.value, ref.id).map { it.toDomain() }
	}

	suspend fun song(ref: ItemRef): Song? = io {
		dao.song(ref.server.value, ref.id)?.toDomain()
	}

	// ── Playlists ───────────────────────────────────────────────────────────

	suspend fun savePlaylists(server: ServerId, playlists: List<Playlist>) = write {
		dao.upsertPlaylists(playlists.map { it.toEntity(server) })
	}

	suspend fun playlists(server: ServerId): List<Playlist> = io {
		dao.playlists(server.value).map { it.toDomain() }
	}

	/** Saves the playlist and its ordered contents, replacing what was there. */
	suspend fun savePlaylist(server: ServerId, playlist: Playlist) = write {
		dao.upsertPlaylists(listOf(playlist.toEntity(server)))
		dao.upsertSongs(playlist.songs.map { it.toEntity(server) })
		dao.clearPlaylistSongs(server.value, playlist.ref.id)
		dao.upsertPlaylistSongs(
			playlist.songs.mapIndexed { position, song ->
				PlaylistSongEntity(
					serverId = server.value,
					playlistId = playlist.ref.id,
					position = position,
					songId = song.ref.id,
				)
			}
		)
	}

	suspend fun playlist(ref: ItemRef): Playlist? = io {
		val row = dao.playlist(ref.server.value, ref.id) ?: return@io null
		row.toDomain().copy(
			songs = dao.songsOfPlaylist(ref.server.value, ref.id).map { it.toDomain() }
		)
	}

	suspend fun songsOfPlaylist(ref: ItemRef): List<Song> = io {
		dao.songsOfPlaylist(ref.server.value, ref.id).map { it.toDomain() }
	}

	// ── Search ──────────────────────────────────────────────────────────────

	/**
	 * Substring match on names and titles.
	 *
	 * Deliberately cruder than the server's search: this only has to be useful
	 * when there is no server to ask, and a full-text index over a mirror that
	 * is only as complete as your browsing history would be effort spent on the
	 * wrong problem.
	 */
	suspend fun search(
		server: ServerId,
		query: String,
		artistCount: Int,
		albumCount: Int,
		songCount: Int,
	): LibrarySelection = io {
		val pattern = "%${query.replace("%", "").replace("_", "")}%"
		LibrarySelection(
			artists = dao.searchArtists(server.value, pattern, artistCount).map { it.toDomain() },
			albums = dao.searchAlbums(server.value, pattern, albumCount).map { it.toDomain() },
			songs = dao.searchSongs(server.value, pattern, songCount).map { it.toDomain() },
		)
	}

	suspend fun saveSelection(server: ServerId, selection: LibrarySelection) = write {
		// Artists from a search have no index bucket of their own; the first
		// letter is what the rail would have put them under anyway.
		//
		// Nor do they carry a root kind — search spans every root — so they
		// land under artists. A category section found this way is mislabelled
		// in the mirror until the Categories list is browsed and rewrites it.
		// Accepted rather than paid for with a lookup per result: search
		// results are a means of reaching something, not a browse listing.
		dao.upsertArtists(selection.artists.map { it.toEntity(server, indexLabelFor(it.name)) })
		dao.upsertAlbums(selection.albums.map { it.toEntity(server) })
		dao.upsertSongs(selection.songs.map { it.toEntity(server) })
	}

	// ── Offline reachability ────────────────────────────────────────────────

	/**
	 * Which artists, albums and playlists have stored audio behind them.
	 *
	 * Takes the cache's keys as a parameter rather than reading the cache, so
	 * the mirror stays ignorant of how bytes are stored — the one direction
	 * that dependency must not run.
	 */
	suspend fun storedFilter(storedSongKeys: Set<String>): StoredFilter = io {
		if (storedSongKeys.isEmpty()) return@io StoredFilter.EMPTY

		val albums = mutableSetOf<String>()
		val playlists = mutableSetOf<String>()
		storedSongKeys.chunked(SQL_CHUNK).forEach { chunk ->
			dao.albumsWithStoredSongs(chunk).forEach { albums += "${it.serverId}/${it.refId}" }
			dao.playlistsWithStoredSongs(chunk).forEach {
				playlists += "${it.serverId}/${it.refId}"
			}
		}

		val artists = mutableSetOf<String>()
		val counts = mutableMapOf<String, Int>()
		albums.chunked(SQL_CHUNK).forEach { chunk ->
			dao.artistAlbumsOf(chunk).forEach { row ->
				val artist = "${row.serverId}/${row.artistId}"
				artists += artist
				counts[artist] = (counts[artist] ?: 0) + 1
			}
		}

		StoredFilter(
			songs = storedSongKeys,
			albums = albums,
			artists = artists,
			playlists = playlists,
			albumCounts = counts,
		)
	}

	/**
	 * Every mirrored album and playlist, to the tracks it holds.
	 *
	 * Keyed and valued in encoded refs, so both sides compare directly against
	 * a cache key. A collection never visited online has no entry at all rather
	 * than an empty one, which is the distinction `fullyStored` turns on.
	 *
	 * Videos are left in. Whether one counts is `Pins.downloadable`'s decision
	 * and it needs the setting, which the mirror has no business reading.
	 */
	suspend fun collectionMembership(): Map<String, List<MemberRow>> = io {
		(dao.albumMembership() + dao.playlistMembership()).groupBy { it.containerKey }
	}

	/**
	 * Server-reported byte sizes for the given cache keys, where the mirror
	 * knows them. Keys never browsed are simply absent.
	 */
	suspend fun songSizes(keys: Collection<String>): Map<String, Long> = io {
		if (keys.isEmpty()) return@io emptyMap()
		buildMap {
			keys.chunked(SQL_CHUNK).forEach { chunk ->
				dao.songSizes(chunk).forEach { put(it.refKey, it.sizeBytes) }
			}
		}
	}

	/** Which of [keys] name videos; see `LibraryDao.videoRefKeys`. */
	suspend fun videoRefKeys(keys: Collection<String>): Set<String> = io {
		if (keys.isEmpty()) return@io emptySet()
		buildSet {
			keys.chunked(SQL_CHUNK).forEach { chunk -> addAll(dao.videoRefKeys(chunk)) }
		}
	}

	// ── Housekeeping ────────────────────────────────────────────────────────

	suspend fun forgetServer(server: ServerId) = write {
		dao.deletePlaylistSongs(server.value)
		dao.deletePlaylists(server.value)
		forgetLibraryRows(server)
	}

	/**
	 * Drops the mirrored hierarchy of one server, keeping its playlists.
	 *
	 * For a move that happened on the server's disk — `promoteAlbum` — after
	 * which every stored id and index bucket below the moved folder names
	 * something that is no longer there. Coarse on purpose: the app cannot tell
	 * which rows moved without re-reading them, and re-reading them is what the
	 * next browse does anyway. Playlists are spared because they reference songs
	 * by id and the ids themselves are unchanged by a promote.
	 */
	suspend fun forgetLibrary(server: ServerId) = write { forgetLibraryRows(server) }

	private suspend fun forgetLibraryRows(server: ServerId) {
		dao.deleteSongs(server.value)
		dao.deleteAlbums(server.value)
		dao.deleteArtists(server.value)
	}

	private suspend fun <T> io(block: suspend () -> T): T =
		withContext(Dispatchers.IO) { block() }

	/** [io], and announces that something changed. */
	private suspend fun write(block: suspend () -> Unit) {
		io(block)
		_revision.update { it + 1 }
	}
}

/**
 * Well under SQLite's variable limit, which is 999 on older Android releases
 * and only larger on newer ones.
 */
private const val SQL_CHUNK = 500

private fun indexLabelFor(name: String): String {
	val first = name.trimStart().firstOrNull()?.uppercaseChar() ?: '#'
	return if (first.isLetter()) first.toString() else "#"
}

// ── Mapping ─────────────────────────────────────────────────────────────────
//
// Written out rather than generated: the domain models carry merge state
// (`refs`) that has no place in storage, so the conversion is lossy in one
// direction by design and a mechanical mapping would hide that.

private fun Artist.toEntity(
	server: ServerId,
	indexLabel: String,
	/** Defaulted for the paths that genuinely cannot know — see saveSelection. */
	contentType: String = LibrarySection.ARTISTS.id,
) = ArtistEntity(
	serverId = server.value,
	id = ref.id,
	name = name,
	albumCount = albumCount,
	coverArtId = coverArt?.id,
	starredAt = starredAt,
	indexLabel = indexLabel,
	contentType = contentType,
)

private fun ArtistEntity.toDomain(): Artist {
	val ref = ItemRef(ServerId(serverId), id)
	return Artist(
		ref = ref,
		name = name,
		albumCount = albumCount,
		coverArt = coverArtId?.let { ItemRef(ServerId(serverId), it) },
		starredAt = starredAt,
	)
}

private fun Album.toEntity(server: ServerId) = AlbumEntity(
	serverId = server.value,
	id = ref.id,
	title = title,
	artistName = artistName,
	artistId = artistRef?.id,
	songCount = songCount,
	videoCount = videoCount,
	duration = duration,
	year = year,
	genre = genre,
	coverArtId = coverArt?.id,
	starredAt = starredAt,
)

private fun AlbumEntity.toDomain(): Album {
	val server = ServerId(serverId)
	return Album(
		ref = ItemRef(server, id),
		title = title,
		artistName = artistName,
		artistRef = artistId?.let { ItemRef(server, it) },
		songCount = songCount,
		videoCount = videoCount,
		duration = duration,
		year = year,
		genre = genre,
		coverArt = coverArtId?.let { ItemRef(server, it) },
		starredAt = starredAt,
	)
}

private fun Song.toEntity(server: ServerId) = SongEntity(
	serverId = server.value,
	id = ref.id,
	title = title,
	artistName = artistName,
	albumArtistName = albumArtistName,
	albumTitle = albumTitle,
	albumId = albumRef?.id,
	track = track,
	discNumber = discNumber,
	year = year,
	duration = duration,
	bitRate = bitRate,
	suffix = suffix,
	contentType = contentType,
	sizeBytes = sizeBytes,
	coverArtId = coverArt?.id,
	starredAt = starredAt,
	isVideo = isVideo,
	season = season,
)

private fun SongEntity.toDomain(): Song {
	val server = ServerId(serverId)
	return Song(
		ref = ItemRef(server, id),
		title = title,
		artistName = artistName,
		albumArtistName = albumArtistName,
		albumTitle = albumTitle,
		albumRef = albumId?.let { ItemRef(server, it) },
		track = track,
		discNumber = discNumber,
		year = year,
		duration = duration,
		bitRate = bitRate,
		suffix = suffix,
		contentType = contentType,
		sizeBytes = sizeBytes,
		coverArt = coverArtId?.let { ItemRef(server, it) },
		starredAt = starredAt,
		isVideo = isVideo,
		season = season,
		// Left at its default, along with transcodedContentType: the mirror
		// stores neither, and false is the answer that still plays — HLS works
		// for every video, it is merely more work for the server than a file
		// that could have been ranged. It no longer costs the cast route
		// either, now that an HLS film casts to a receiver that can reach the
		// server; it only costs the cheaper tier. Both facts want the network
		// anyway.
		nativeSeek = false,
	)
}

private fun Playlist.toEntity(server: ServerId) = PlaylistEntity(
	serverId = server.value,
	id = ref.id,
	name = name,
	comment = comment,
	owner = owner,
	isPublic = isPublic,
	songCount = songCount,
	duration = duration,
)

private fun PlaylistEntity.toDomain() = Playlist(
	ref = ItemRef(ServerId(serverId), id),
	name = name,
	comment = comment,
	owner = owner,
	isPublic = isPublic,
	songCount = songCount,
	duration = duration,
)
