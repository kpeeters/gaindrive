package org.gaindrive.android.data.browse

import org.gaindrive.android.data.model.Album
import org.gaindrive.android.data.model.AlbumDetail
import org.gaindrive.android.data.model.Artist
import org.gaindrive.android.data.model.ArtistIndex
import org.gaindrive.android.data.model.ItemRef
import org.gaindrive.android.data.model.LibrarySelection
import org.gaindrive.android.data.model.ServerConfig
import org.gaindrive.android.data.model.ServerId
import org.gaindrive.android.net.SubsonicApi

/**
 * The two ways of reading the same hierarchy.
 *
 * A Subsonic server exposes artists, albums and tracks twice: once derived from
 * the files' tags, and once from the directory tree they sit in. gaindrive
 * builds both from the tree, so the two agree and the choice does not matter.
 * Elsewhere it matters a great deal — a library whose tags are patchy browses
 * correctly through its folders and badly through its tags, which is what this
 * interface exists to switch between.
 *
 * Only the network-to-domain half lives here. Fanning out across servers,
 * writing the mirror and merging the results are the repository's, and are the
 * same either way — which is the point of putting the fork here rather than
 * inside each of those methods, where the two arms would have to be kept in step
 * by hand.
 *
 * Takes [SubsonicApi] rather than `SubsonicClient` so the implementations can be
 * driven by the MockWebServer harness the DTO tests already use; the client
 * implements the interface by delegation, so callers pass it unchanged.
 */
interface BrowseSource {

	/** The top-level list, narrowed by [request]. */
	suspend fun indexes(
		api: SubsonicApi,
		server: ServerId,
		request: RootRequest,
	): List<ArtistIndex>

	/** One artist's albums. */
	suspend fun albums(api: SubsonicApi, ref: ItemRef): List<Album>

	/** One artist on its own, for the header of their album list. */
	suspend fun artist(api: SubsonicApi, ref: ItemRef): Artist?

	/** One album with its tracks, flat and ordered by disc. */
	suspend fun albumDetail(api: SubsonicApi, ref: ItemRef): AlbumDetail?

	/**
	 * Search. Part of this interface rather than shared because the two
	 * hierarchies answer with their own ids, and a result is only useful if the
	 * id it carries is one the rest of the mode can open.
	 */
	suspend fun search(
		api: SubsonicApi,
		server: ServerId,
		query: String,
		artistCount: Int,
		albumCount: Int,
		songCount: Int,
	): LibrarySelection
}

/** Which of the two this server is configured to use. */
val ServerConfig.browseSource: BrowseSource
	get() = if (browseByFolder) FolderSource else Id3Source
