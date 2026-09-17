package org.gaindrive.android.data.cache

import android.content.Context
import android.util.Log
import coil3.Uri
import coil3.map.Mapper
import coil3.request.Options
import coil3.toUri
import dagger.hilt.android.qualifiers.ApplicationContext
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.withContext
import okhttp3.OkHttpClient
import okhttp3.Request
import org.gaindrive.android.data.ServerRegistry
import org.gaindrive.android.data.model.ItemRef
import org.gaindrive.android.net.SubsonicClient
import org.gaindrive.android.net.SubsonicClientFactory
import java.io.File
import javax.inject.Inject
import javax.inject.Singleton

/**
 * Cover art and artist portraits kept for what the user has pinned.
 *
 * Lives in `filesDir/art`, not `cacheDir`, for the same reason the audio cache
 * does: Android empties `cacheDir` under storage pressure, and a downloaded
 * album whose cover vanishes at the moment its owner goes offline is precisely
 * the failure this exists to prevent. Coil's own disk cache remains where it
 * is, because art for something *not* pinned genuinely is disposable.
 *
 * One file per picture, fetched at [ART_PX], which Coil downscales for the
 * list, portrait and notification sizes. Four files per album would be four
 * fetches of a picture that decodes perfectly well from one.
 */
@Singleton
class PinnedArt @Inject constructor(
	@ApplicationContext context: Context,
) {

	private val dir = File(context.filesDir, "art")

	/**
	 * Lookup key to file, for [PinnedArtMapper].
	 *
	 * In memory rather than a `File.exists()` probe, and replaced whole rather
	 * than mutated, for the same reason as [PinnedKeys]: Coil maps the data of
	 * every request before it reads its memory cache, on the main dispatcher,
	 * so a disk stat here would be one per row of a scrolling list.
	 */
	@Volatile
	var files: Map<String, File> = emptyMap()
		private set

	/** Set before the pins that produced it are published, never after. */
	fun publish(index: Map<String, File>) {
		files = index
	}

	/**
	 * No `exists()` here on purpose: this runs on the main dispatcher for
	 * every row of a scrolling list. [index] filters when it is built, and
	 * rebuilds once the downloads land.
	 */
	fun fileFor(url: String): File? = ArtKeys.lookupKey(url)?.let { files[it] }

	fun path(name: String): File = File(dir, name)

	fun has(name: String): Boolean = path(name).exists()

	/**
	 * Written to a neighbour and renamed, so a kill mid-write cannot leave a
	 * truncated file that [has] would then report as present for ever.
	 */
	fun write(name: String, bytes: ByteArray) {
		dir.mkdirs()
		val tmp = File(dir, "$name.tmp")
		tmp.writeBytes(bytes)
		if (!tmp.renameTo(path(name))) tmp.delete()
	}

	/**
	 * Drops every file except [names], which is how unpinning frees them.
	 *
	 * Partial writes are left alone: one belongs to a fetch still running, and
	 * deleting it under the writer only costs that picture a second attempt.
	 */
	fun retain(names: Set<String>) {
		val gone = dir.listFiles()
			?.filterNot { it.name in names || it.name.endsWith(".tmp") } ?: return
		gone.forEach { it.delete() }
		if (gone.isNotEmpty()) Log.i(TAG, "dropped ${gone.size} pinned image(s)")
	}

	fun clear() {
		files = emptyMap()
		dir.listFiles()?.forEach { it.delete() }
	}

	fun sizeBytes(): Long = dir.listFiles()?.sumOf { it.length() } ?: 0L

	companion object {
		private const val TAG = "PinnedArt"

		/**
		 * A rung on the server's scaling ladder, and the size the album hero
		 * already asks for, so pinning one costs no extra work server-side.
		 */
		const val ART_PX = 800
	}
}

/**
 * Serves a pinned file in place of the network URL that named it.
 *
 * A [Mapper] rather than a `Fetcher` because it is a lookup and a type change,
 * nothing more. Its usual drawback, that mapping changes what Coil keys on,
 * does not apply here: `artCaching` sets both keys explicitly from the URL, so
 * a pinned load and an unpinned one of the same picture still share an entry.
 */
class PinnedArtMapper(private val store: PinnedArt) : Mapper<String, Uri> {
	override fun map(data: String, options: Options): Uri? =
		store.fileFor(data)?.let { "file://${it.absolutePath}".toUri() }
}

/**
 * Fetches the art a pin covers, once.
 *
 * Reaches the servers through [SubsonicClientFactory] rather than through
 * `LibraryRepository.coverUrls`, for the reason `StreamUrls` gives for the
 * same choice: that class writes the mirror `PinRepository` reads, and a
 * dependency back to it would close the loop.
 */
@Singleton
class ArtDownloader @Inject constructor(
	private val http: OkHttpClient,
	private val registry: ServerRegistry,
	private val clients: SubsonicClientFactory,
	private val store: PinnedArt,
) {

	/**
	 * Lookup key to file, for every ref whose file is actually here.
	 *
	 * Here rather than in `PinRepository` because the key has to be derived
	 * from the URL a server would build, and this is the side that holds the
	 * clients. Filtered at build time rather than at lookup time so that the
	 * mapper never touches the disk.
	 */
	suspend fun index(refs: Collection<ItemRef>): Map<String, File> =
		withContext(Dispatchers.IO) {
			refs.groupBy { it.server }.flatMap { (server, wanted) ->
				val config = registry.get(server) ?: return@flatMap emptyList()
				val client = clients.clientFor(config)
				wanted.mapNotNull { ref ->
					val file = store.path(ArtKeys.fileName(ref))
					if (!file.exists()) return@mapNotNull null
					ArtKeys.lookupKey(client, ref.id)?.let { it to file }
				}
			}.toMap()
		}

	/**
	 * Skips what is already here, so this is safe to call on every pin change,
	 * which is what makes a portrait the server had not yet resolved arrive on
	 * a later pass rather than never.
	 */
	suspend fun fetchMissing(refs: Collection<ItemRef>) = withContext(Dispatchers.IO) {
		// Grouped by server because resolving one means reading DataStore and
		// decrypting a password, which is far too expensive to repeat per
		// picture. The same reasoning gave `CoverUrls` its per-screen map.
		refs.filterNot { store.has(ArtKeys.fileName(it)) }
			.groupBy { it.server }
			.forEach { (server, wanted) ->
				val config = registry.get(server) ?: return@forEach
				val client = clients.clientFor(config)
				wanted.forEach { ref ->
					runCatching { fetch(client, ref, ArtKeys.fileName(ref)) }
						.onFailure { Log.w(TAG, "art for ${ref.encode()}: ${it.message}") }
				}
			}
	}

	private fun fetch(client: SubsonicClient, ref: ItemRef, name: String) {
		val url = client.url(
			"getCoverArt",
			mapOf("id" to ref.id, "size" to PinnedArt.ART_PX.toString()),
		)
		http.newCall(Request.Builder().url(url).build()).execute().use { response ->
			// A 404 is the ordinary answer for an artist whose portrait the
			// server has not found, so it must not be logged as a failure.
			if (!response.isSuccessful) return
			// Subsonic reports its own errors as a 200 carrying JSON, which
			// would otherwise be written out as an image file that never
			// decodes and that `has` would then treat as done.
			val type = response.header("Content-Type").orEmpty()
			if (!type.startsWith("image/")) return
			val bytes = response.body?.bytes() ?: return
			if (bytes.size > MAX_BYTES) return
			store.write(name, bytes)
		}
	}

	private companion object {
		const val TAG = "ArtDownloader"

		/** A cover is tens of kilobytes; anything this large is not one. */
		const val MAX_BYTES = 8 * 1024 * 1024
	}
}
