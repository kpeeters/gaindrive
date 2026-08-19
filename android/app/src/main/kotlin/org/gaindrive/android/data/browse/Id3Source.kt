package org.gaindrive.android.data.browse

import org.gaindrive.android.data.model.Album
import org.gaindrive.android.data.model.AlbumDetail
import org.gaindrive.android.data.model.Artist
import org.gaindrive.android.data.model.ArtistIndex
import org.gaindrive.android.data.model.ItemRef
import org.gaindrive.android.data.model.LibrarySelection
import org.gaindrive.android.data.model.ServerId
import org.gaindrive.android.data.toDomain
import org.gaindrive.android.net.SubsonicApi
import org.gaindrive.android.net.requireOk

/**
 * The hierarchy as the server's tags describe it, which is what this app has
 * always used and remains the default.
 *
 * Every method is one request and one mapper — there is nothing to arrange,
 * because these endpoints already answer in the shape the screens want.
 */
object Id3Source : BrowseSource {

	override suspend fun indexes(
		api: SubsonicApi,
		server: ServerId,
		request: RootRequest,
	): List<ArtistIndex> =
		api.getArtists(null, request.contentType)
			.requireOk().artists?.index.orEmpty()
			.map { it.toDomain(server) }

	override suspend fun albums(api: SubsonicApi, ref: ItemRef): List<Album> =
		api.getArtist(ref.id).requireOk().artist?.album.orEmpty()
			.map { it.toDomain(ref.server) }

	override suspend fun artist(api: SubsonicApi, ref: ItemRef): Artist? =
		api.getArtist(ref.id).requireOk().artist?.toDomain(ref.server)

	override suspend fun albumDetail(api: SubsonicApi, ref: ItemRef): AlbumDetail? {
		val dto = api.getAlbum(ref.id).requireOk().album ?: return null
		return AlbumDetail(
			album = dto.toDomain(ref.server),
			songs = dto.song.map { it.toDomain(ref.server) },
		)
	}

	override suspend fun search(
		api: SubsonicApi,
		server: ServerId,
		query: String,
		artistCount: Int,
		albumCount: Int,
		songCount: Int,
	): LibrarySelection =
		api.search3(query, artistCount, albumCount, songCount)
			.requireOk().searchResult3?.toDomain(server) ?: LibrarySelection()
}
