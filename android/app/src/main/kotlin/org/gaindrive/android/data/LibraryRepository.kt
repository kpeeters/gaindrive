package org.gaindrive.android.data

import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.flow.first
import kotlinx.coroutines.withContext
import org.gaindrive.android.data.model.Album
import org.gaindrive.android.data.model.AlbumDetail
import org.gaindrive.android.data.model.AlbumNotes
import org.gaindrive.android.data.model.Artist
import org.gaindrive.android.data.model.ArtistIndex
import org.gaindrive.android.data.model.ArtistInfo
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
import javax.inject.Inject
import javax.inject.Singleton

/**
 * Reads the music library. Single-server queries only at this point;
 * sub-phase 2c adds the all-servers fan-out on top of these.
 *
 * Everything returns domain models carrying an [ItemRef], so a caller can
 * always tell which server an item came from.
 */
@Singleton
class LibraryRepository @Inject constructor(
	private val registry: ServerRegistry,
	private val clients: SubsonicClientFactory,
) {

	suspend fun artistIndexes(server: ServerId, personalOnly: Boolean = false): List<ArtistIndex> =
		onServer(server) { client ->
			// Positional: SubsonicClient reaches the API through interface
			// delegation, so named arguments lean on generated parameter names.
			client.getArtists(if (personalOnly) "true" else null)
				.requireOk().artists?.index.orEmpty()
				.map { it.toDomain(server) }
				// Buckets with no artists are noise in a sticky-header list.
				.filter { it.artists.isNotEmpty() }
		}

	suspend fun albumsOfArtist(artist: ItemRef): List<Album> =
		onServer(artist.server) { client ->
			client.getArtist(artist.id).requireOk().artist?.album.orEmpty()
				.map { it.toDomain(artist.server) }
		}

	suspend fun artist(ref: ItemRef): Artist? =
		onServer(ref.server) { client ->
			client.getArtist(ref.id).requireOk().artist?.toDomain(ref.server)
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
	 * Album with its tracks and, when available, its notes. The notes call is
	 * a separate request and is allowed to fail on its own: a missing
	 * biography must not cost the user their track list.
	 */
	suspend fun albumDetail(album: ItemRef): AlbumDetail? =
		onServer(album.server) { client ->
			val dto = client.getAlbum(album.id).requireOk().album ?: return@onServer null
			AlbumDetail(
				album = dto.toDomain(album.server),
				songs = dto.song.map { it.toDomain(album.server) },
				notes = runCatching {
					client.getAlbumInfo2(album.id).requireOk().albumInfo2?.toDomain()
				}.getOrNull()?.takeIf { !it.isEmpty },
			)
		}

	suspend fun albumNotes(album: ItemRef): AlbumNotes? =
		onServer(album.server) { client ->
			client.getAlbumInfo2(album.id).requireOk().albumInfo2?.toDomain()
		}

	suspend fun playlists(server: ServerId): List<Playlist> =
		onServer(server) { client ->
			client.getPlaylists().requireOk().playlists?.playlist.orEmpty()
				.map { it.toDomain(server) }
		}

	suspend fun playlist(ref: ItemRef): Playlist? =
		onServer(ref.server) { client ->
			client.getPlaylist(ref.id).requireOk().playlist?.toDomain(ref.server)
		}

	suspend fun search(
		server: ServerId,
		query: String,
		artistCount: Int = 20,
		albumCount: Int = 20,
		songCount: Int = 50,
	): LibrarySelection = onServer(server) { client ->
		client.search3(query, artistCount, albumCount, songCount)
			.requireOk().searchResult3?.toDomain(server) ?: LibrarySelection()
	}

	suspend fun starred(server: ServerId): LibrarySelection =
		onServer(server) { client ->
			client.getStarred2().requireOk().starred2?.toDomain(server) ?: LibrarySelection()
		}

	/**
	 * Recently played. A gaindrive extension, so a server that does not
	 * implement it yields an empty list rather than an error — the section
	 * simply has nothing in it.
	 */
	suspend fun recentSongs(server: ServerId, size: Int = 50): List<Song> =
		runCatching {
			onServer(server) { client ->
				client.getRecentSongs(size).requireOk().recentSongs?.song.orEmpty()
					.map { it.toDomain(server) }
			}
		}.getOrDefault(emptyList())

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
	}

	suspend fun addToPlaylist(playlist: ItemRef, songId: String) {
		onServer(playlist.server) { client ->
			client.updatePlaylist(playlist.id, listOf(songId), emptyList()).requireOk()
		}
	}

	/** Removes by position; the caller must not let indices shift under it. */
	suspend fun removeFromPlaylist(playlist: ItemRef, index: Int) {
		onServer(playlist.server) { client ->
			client.updatePlaylist(playlist.id, emptyList(), listOf(index)).requireOk()
		}
	}

	suspend fun deletePlaylist(playlist: ItemRef) {
		onServer(playlist.server) { client -> client.deletePlaylist(playlist.id).requireOk() }
	}

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
}
