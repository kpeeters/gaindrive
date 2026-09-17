package org.gaindrive.android.data

import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.withContext
import org.gaindrive.android.data.local.LocalLibrary
import org.gaindrive.android.data.model.ChapterList
import org.gaindrive.android.data.model.ChapterSource
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
 * Shaped like [CaptionTracks]: a per-item lookup on the load path. It stays
 * out of [LibraryRepository] because none of that class's machinery, the
 * offline decision or the fan-out across servers, has anything to say about
 * it.
 *
 * The mirror is the one exception, and only as a fallback. The markers the
 * album listing stored are the sidecar ones, so they are the right answer for
 * a downloaded recording being played with no network, and the wrong answer
 * whenever the server can be asked: only `getChapters` sees a video's
 * container chapters and a list somebody edited a moment ago.
 */
@Singleton
class ChapterTracks @Inject constructor(
	private val registry: ServerRegistry,
	private val clients: SubsonicClientFactory,
	private val local: LocalLibrary,
) {

	/**
	 * Never throws and never returns null. A recording with no markers, a
	 * server too old for the endpoint and a request that failed are the same
	 * thing here — no chapter list to draw — and none of them is a reason to
	 * refuse to play the item.
	 */
	suspend fun chaptersFor(ref: ItemRef): ChapterList = withContext(Dispatchers.IO) {
		val config = registry.get(ref.server)
		val live = config?.let { server ->
			runCatching {
				clients.clientFor(server).getChapters(ref.id).requireOk().chapters?.toDomain()
			}.getOrNull()
		}
		// Only when the server could not be asked, never when it answered with
		// nothing. A server that says "no markers" is right, including about a
		// list somebody has just deleted, and preferring stored rows over that
		// would keep the deleted ones on screen until the album was next
		// opened. Offline the request fails rather than answering empty, so
		// this still covers the case it exists for.
		live ?: local.chaptersOfSong(ref).let { stored ->
			// Nothing stored is `NONE`, not an empty sidecar: the source is
			// read as a claim about where a list came from, and there is no
			// list here to have come from anywhere.
			if (stored.isEmpty()) ChapterList()
			else ChapterList(stored, ChapterSource.SIDECAR)
		}
	}
}
