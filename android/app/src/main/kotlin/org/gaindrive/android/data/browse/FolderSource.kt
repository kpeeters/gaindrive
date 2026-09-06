package org.gaindrive.android.data.browse

import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.async
import kotlinx.coroutines.awaitAll
import kotlinx.coroutines.coroutineScope
import org.gaindrive.android.data.model.Album
import org.gaindrive.android.data.model.AlbumDetail
import org.gaindrive.android.data.model.Artist
import org.gaindrive.android.data.model.ArtistIndex
import org.gaindrive.android.data.model.ItemRef
import org.gaindrive.android.data.model.LibrarySelection
import org.gaindrive.android.data.model.ServerId
import org.gaindrive.android.data.toAlbum
import org.gaindrive.android.data.toArtist
import org.gaindrive.android.data.toDomain
import org.gaindrive.android.net.DirectoryDto
import org.gaindrive.android.net.SongDto
import org.gaindrive.android.net.SubsonicApi
import org.gaindrive.android.net.SubsonicException
import org.gaindrive.android.net.requireOk

/**
 * The hierarchy as the directory tree describes it.
 *
 * `getMusicDirectory` does all three jobs — an artist's albums and an album's
 * tracks are the same request against different ids — so the work here is not
 * fetching but deciding what a listing *is*, and assembling an album that was
 * split across disc subfolders back into one track list.
 */
object FolderSource : BrowseSource {

	override suspend fun indexes(
		api: SubsonicApi,
		server: ServerId,
		request: RootRequest,
	): List<ArtistIndex> =
		api.getIndexes(request.musicFolderId, request.contentType, request.personalParam)
			.requireOk().indexes?.index.orEmpty()
			.map { it.toDomain(server) }

	override suspend fun albums(api: SubsonicApi, ref: ItemRef): List<Album> =
		directory(api, ref)?.child.orEmpty()
			.filter { it.isDir }
			.map { it.toAlbum(ref.server) }

	override suspend fun artist(api: SubsonicApi, ref: ItemRef): Artist? =
		directory(api, ref)?.toArtist(ref.server)

	/**
	 * An album's tracks, whether they sit in the folder itself or one level down
	 * in disc subfolders.
	 *
	 * The fallback to the tag hierarchy at either end is for a reference that
	 * came from somewhere other than this mode's own browsing — the offline
	 * mirror written before the switch was flipped, a starred album, a
	 * back-stack entry. It is a safety net and not the mechanism: search already
	 * answers in folder ids (see [search]), because on a server whose two id
	 * spaces are both integers a wrong-space id can resolve to a real album and
	 * no error would ever be raised.
	 */
	override suspend fun albumDetail(api: SubsonicApi, ref: ItemRef): AlbumDetail? {
		val dir = try {
			api.getMusicDirectory(ref.id).requireOk().directory
		} catch (e: SubsonicException) {
			if (e.code == SubsonicException.NOT_FOUND) return Id3Source.albumDetail(api, ref)
			throw e
		} ?: return null

		val own = dir.child.filterNot { it.isDir }
		// Subdirectories are ignored once the folder holds tracks of its own: a
		// bonus-material or artwork folder sitting beside them must not turn
		// into a phantom disc.
		val songs = if (own.isNotEmpty()) own else discsOf(api, dir)

		// The id resolved but named nothing playable. On a server whose album
		// ids and folder ids collide numerically, that is what an id from the
		// wrong hierarchy looks like when it happens to hit something.
		if (songs.isEmpty()) return Id3Source.albumDetail(api, ref)

		val albumRef = ItemRef(ref.server, dir.id)
		val mapped = songs.map { it.toDomain(ref.server, albumRef = albumRef) }
		return AlbumDetail(album = dir.toAlbum(ref.server, mapped), songs = mapped)
	}

	override suspend fun search(
		api: SubsonicApi,
		server: ServerId,
		query: String,
		artistCount: Int,
		albumCount: Int,
		songCount: Int,
		chapterCount: Int,
	): LibrarySelection =
		api.search2(query, artistCount, albumCount, songCount, chapterCount)
			.requireOk().searchResult2?.toDomain(server) ?: LibrarySelection()

	/**
	 * The tracks of an album whose discs are subfolders, fetched concurrently
	 * and numbered by the order those folders sort in.
	 *
	 * One level only. Anything deeper is not a disc, and the cap keeps a library
	 * root that someone navigated into from becoming a request per entry.
	 */
	private suspend fun discsOf(api: SubsonicApi, dir: DirectoryDto): List<SongDto> {
		val discs = dir.child.filter { it.isDir }.sortedWith(DISC_ORDER).take(MAX_DISCS)
		if (discs.isEmpty()) return emptyList()

		val listings = coroutineScope {
			discs.map { disc ->
				async(Dispatchers.IO) {
					api.getMusicDirectory(disc.id).requireOk()
						.directory?.child.orEmpty().filterNot { it.isDir }
				}
			}.awaitAll()
		}
		return flattenDiscs(listings)
	}

	private suspend fun directory(api: SubsonicApi, ref: ItemRef): DirectoryDto? =
		api.getMusicDirectory(ref.id).requireOk().directory
}
