package org.gaindrive.android.data

import kotlinx.coroutines.CancellationException
import kotlinx.coroutines.flow.first
import org.gaindrive.android.data.model.ItemRef
import org.gaindrive.android.net.SubsonicClientFactory
import org.gaindrive.android.net.requireOk
import org.gaindrive.android.net.userMessage
import javax.inject.Inject
import javax.inject.Singleton

/**
 * What acting on a [TrackLink] came to. [Album] carries exactly what
 * `Route.Album` wants — the album to open, and the track to start once its
 * listing has loaded — because opening the album *is* the action: playing the
 * bare `getSong` entry would work, but the album route's own documentation
 * records why the listing is read again (`nativeSeek`), and it hands the queue
 * the rest of the album besides. The same path a chapter hit in search takes.
 */
sealed interface TrackLinkResult {
	data class Album(
		val albumRef: String,
		val albumTitle: String,
		val songRef: String,
		val positionMs: Long,
	) : TrackLinkResult

	data class Error(val message: String) : TrackLinkResult
}

/**
 * Turns a track link into the album to open.
 *
 * Separate from [LibraryRepository] for [UrlFetchRepository]'s reasons: a link
 * lands on exactly one server, nothing here fans out or has an offline story
 * beyond failing with a message worth reading.
 */
@Singleton
class TrackLinkResolver @Inject constructor(
	private val registry: ServerRegistry,
	private val clients: SubsonicClientFactory,
) {

	suspend fun resolve(link: TrackLink): TrackLinkResult {
		// Matched against every server, enabled or not, so a disabled one is
		// reported as what it is. "Not one of your servers" about a server
		// sitting right there in Settings would send the reader to re-check a
		// URL that is fine.
		val config = registry.servers.first().firstOrNull { serverMatchesLink(it.url, link) }
			?: return TrackLinkResult.Error(
				"This link is for ${link.authority}, which is not one of your servers.",
			)
		if (!config.enabled) {
			return TrackLinkResult.Error(
				"This link is for ${config.name}, which is disabled in Settings.",
			)
		}

		return try {
			val song = clients.clientFor(config).getSong(link.trackId).requireOk().song
				?: return TrackLinkResult.Error("The server did not return that track.")
			// The ID3 album wins over the folder parent, as SongDto.toDomain
			// documents; on a gaindrive server they are the same folder row.
			val albumId = song.albumId ?: song.parent
				?: return TrackLinkResult.Error("That track belongs to no album the server names.")
			TrackLinkResult.Album(
				albumRef = ItemRef(config.id, albumId).encode(),
				albumTitle = song.album.orEmpty(),
				songRef = ItemRef(config.id, song.id).encode(),
				positionMs = link.positionMs,
			)
		} catch (e: CancellationException) {
			throw e
		} catch (e: Exception) {
			// A song id is a rowid on the server and is reassigned when its
			// music DB is rebuilt, so "not found" is a link that has outlived
			// one — worth a message rather than a silent shrug, since from the
			// outside it reads as the feature being broken.
			TrackLinkResult.Error(e.userMessage())
		}
	}
}
