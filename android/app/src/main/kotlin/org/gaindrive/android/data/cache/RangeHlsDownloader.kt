package org.gaindrive.android.data.cache

import android.net.Uri
import android.util.Log
import androidx.media3.common.C
import androidx.media3.common.util.UriUtil
import androidx.media3.datasource.DataSourceInputStream
import androidx.media3.datasource.DataSpec
import androidx.media3.datasource.cache.CacheDataSource
import androidx.media3.datasource.cache.CacheWriter
import androidx.media3.exoplayer.hls.playlist.HlsMediaPlaylist
import androidx.media3.exoplayer.hls.playlist.HlsPlaylistParser
import androidx.media3.exoplayer.offline.DownloadRequest
import androidx.media3.exoplayer.offline.Downloader
import androidx.media3.exoplayer.offline.DownloaderFactory
import java.io.IOException
import java.io.InterruptedIOException

/**
 * Stores part of a film's soundtrack HLS: its playlist, its init segment and
 * the media segments overlapping one time range.
 *
 * Media3's own `HlsDownloader` always takes the whole playlist, which is the
 * wrong unit for one chapter of a concert. This is that downloader cut down to
 * one rendition and one range, on Media3's [CacheWriter] like the rest. A whole
 * film is simply a range that covers all of it.
 *
 * The range travels in [DownloadRequest.data] ([encodeRange]), which is also
 * how [RangeDownloaderFactory] tells these requests from a song's.
 */
class RangeHlsDownloader(
	private val request: DownloadRequest,
	private val cacheFactory: CacheDataSource.Factory,
	private val pinned: PinnedKeys,
	private val keys: HlsKeys,
) : Downloader {

	@Volatile
	private var writer: CacheWriter? = null

	@Volatile
	private var cancelled = false

	override fun download(progressListener: Downloader.ProgressListener?) {
		val (startMs, endMs) = decodeRange(request.data)
			?: throw IOException("Download ${request.id} names no range")
		val source = cacheFactory.createDataSourceForDownloading()

		write(source, DataSpec(request.uri))
		val parts = partsOf(readPlaylist(source), startMs, endMs)
		var bytes = 0L
		parts.forEachIndexed { i, uri ->
			bytes += write(source, DataSpec(uri))
			progressListener?.onProgress(
				C.LENGTH_UNSET.toLong(),
				bytes,
				100f * (i + 1) / parts.size,
			)
		}
	}

	override fun cancel() {
		cancelled = true
		writer?.cancel()
	}

	/**
	 * Deletes what this range stored, except what another pin still covers.
	 *
	 * Neighbouring chapters share the segment their boundary falls in, and
	 * every range of a film shares its playlist and init segment, so removing
	 * all of ours would break the pins left behind. `PinRepository` updates
	 * [PinnedKeys] before asking for the removal, which is what makes
	 * "still covered" a plain lookup here.
	 *
	 * The keys come from the stored playlist. Without one there is nothing to
	 * remove that can be named, and anything left over is ordinary unpinned
	 * cache that eviction reclaims.
	 */
	override fun remove() {
		val cache = cacheFactory.cache ?: return
		val range = decodeRange(request.data) ?: return
		// No upstream: a playlist that is not stored must fail here, not be
		// fetched from the server just to be deleted.
		val stored = CacheDataSource.Factory()
			.setCache(cache)
			.setCacheKeyFactory(keys.factory)
			.createDataSource()
		val playlist = try {
			readPlaylist(stored)
		} catch (e: IOException) {
			Log.i(TAG, "remove ${request.id}: no stored playlist (${e.message})")
			return
		}
		val owned = (listOf(request.uri) + partsOf(playlist, range.first, range.second))
			.mapNotNull { keys.keyFor(it) }
		owned.filterNot(pinned::isPinned).forEach { key ->
			runCatching { cache.removeResource(key) }
				.onFailure { Log.w(TAG, "remove ${request.id}: could not remove $key", it) }
		}
	}

	/** Returns the bytes now cached for [spec], counting any already there. */
	private fun write(source: CacheDataSource, spec: DataSpec): Long {
		if (cancelled) throw InterruptedIOException("Download ${request.id} cancelled")
		var cached = 0L
		val w = CacheWriter(source, spec, null) { _, bytesCached, _ -> cached = bytesCached }
		writer = w
		try {
			w.cache()
		} finally {
			writer = null
		}
		return cached
	}

	private fun readPlaylist(source: CacheDataSource): HlsMediaPlaylist {
		val parsed = DataSourceInputStream(source, DataSpec(request.uri)).use {
			HlsPlaylistParser().parse(request.uri, it)
		}
		return parsed as? HlsMediaPlaylist
			?: throw IOException("Download ${request.id}: not a media playlist")
	}

	/** The init segment and every media segment overlapping the range. */
	private fun partsOf(playlist: HlsMediaPlaylist, startMs: Long, endMs: Long): List<Uri> {
		val startUs = startMs * 1000
		val endUs = if (endMs > Long.MAX_VALUE / 1000) Long.MAX_VALUE else endMs * 1000
		val segments = playlist.segments.filter {
			it.relativeStartTimeUs < endUs && it.relativeStartTimeUs + it.durationUs > startUs
		}
		val inits = segments.mapNotNull { it.initializationSegment }.distinctBy { it.url }
		return (inits + segments).map { UriUtil.resolveToUri(playlist.baseUri, it.url) }
	}

	companion object {
		private const val TAG = "RangeHlsDownloader"

		fun encodeRange(startMs: Long, endMs: Long): ByteArray =
			"$startMs-$endMs".toByteArray(Charsets.UTF_8)

		/** Null for a request that carries no range, which is every song's. */
		fun decodeRange(data: ByteArray): Pair<Long, Long>? {
			if (data.isEmpty()) return null
			val parts = String(data, Charsets.UTF_8).split('-')
			if (parts.size != 2) return null
			val start = parts[0].toLongOrNull() ?: return null
			val end = parts[1].toLongOrNull() ?: return null
			return if (start >= 0 && end > start) start to end else null
		}
	}
}

/**
 * Hands requests that carry a range to [RangeHlsDownloader], and everything
 * else to Media3's own factory, which is what downloads a song.
 */
class RangeDownloaderFactory(
	private val fallback: DownloaderFactory,
	private val cacheFactory: CacheDataSource.Factory,
	private val pinned: PinnedKeys,
	private val keys: HlsKeys,
) : DownloaderFactory {

	override fun createDownloader(request: DownloadRequest): Downloader =
		if (RangeHlsDownloader.decodeRange(request.data) != null) {
			RangeHlsDownloader(request, cacheFactory, pinned, keys)
		} else {
			fallback.createDownloader(request)
		}
}
