package org.gaindrive.android.data

import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.withContext
import org.gaindrive.android.data.model.ChapterList
import org.gaindrive.android.data.model.ItemRef
import org.gaindrive.android.net.SubsonicClientFactory
import org.gaindrive.android.net.requireOk
import javax.inject.Inject
import javax.inject.Singleton

/**
 * The chapter markers inside the item being played, read from the file itself.
 *
 * The playback-path half of chapters. Its browse counterpart is
 * [LibraryRepository.albumChapters], and the split is the server's: this asks
 * `getChapters`, which opens the media and is therefore right about a list
 * somebody edited a moment ago and about a video's own container chapters,
 * while the listing asks the scan's index, which is cheap enough to want on
 * every album open. A player needs the first; a listing can only afford the
 * second.
 *
 * Shaped like [CaptionTracks] and for the same reasons: a per-item lookup, on
 * the load path, of something no mirror holds. It stays out of
 * [LibraryRepository] because none of that class's machinery — the offline
 * decision, the Room mirror, the fan-out across servers — has anything to say
 * about it.
 */
@Singleton
class ChapterTracks @Inject constructor(
	private val registry: ServerRegistry,
	private val clients: SubsonicClientFactory,
) {

	/**
	 * Never throws and never returns null. A recording with no markers, a
	 * server too old for the endpoint and a request that failed are the same
	 * thing here — no chapter list to draw — and none of them is a reason to
	 * refuse to play the item.
	 */
	suspend fun chaptersFor(ref: ItemRef): ChapterList = withContext(Dispatchers.IO) {
		val config = registry.get(ref.server) ?: return@withContext ChapterList()
		val client = clients.clientFor(config)
		runCatching {
			client.getChapters(ref.id).requireOk().chapters?.toDomain()
		}.getOrNull() ?: ChapterList()
	}
}
