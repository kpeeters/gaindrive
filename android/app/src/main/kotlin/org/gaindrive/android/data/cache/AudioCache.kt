package org.gaindrive.android.data.cache

import androidx.media3.common.C
import androidx.media3.datasource.DataSource
import androidx.media3.datasource.cache.CacheDataSource
import androidx.media3.datasource.cache.ContentMetadata
import androidx.media3.datasource.cache.SimpleCache
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.channels.Channel
import kotlinx.coroutines.delay
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.asStateFlow
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext
import org.gaindrive.android.data.SettingsStore
import org.gaindrive.android.data.local.LocalLibrary
import org.gaindrive.android.data.model.AudioQuality
import javax.inject.Inject
import javax.inject.Singleton

/**
 * The audio byte cache: what is stored, how much of it there is, and the
 * [DataSource.Factory] that fills it.
 *
 * The Media3 [SimpleCache] is the only record of what has been stored — it
 * already tracks cached ranges and total size per key, so none of that is
 * mirrored into a database. Room holds metadata and pins; this holds bytes.
 */
@Singleton
class AudioCache @Inject constructor(
	private val cache: SimpleCache,
	private val evictor: PinAwareEvictor,
	private val pinned: PinnedKeys,
	private val local: LocalLibrary,
	settings: SettingsStore,
	scope: CoroutineScope,
) {

	private val _usedBytes = MutableStateFlow(0L)

	/** Everything the cache is holding, pinned or not. */
	val usedBytes: StateFlow<Long> = _usedBytes.asStateFlow()

	private val _pinnedBytes = MutableStateFlow(0L)

	/** The part of [usedBytes] that eviction and flushing will not reclaim. */
	val pinnedBytes: StateFlow<Long> = _pinnedBytes.asStateFlow()

	private val _cachedKeys = MutableStateFlow<Set<String>>(emptySet())

	/**
	 * Keys held in full, i.e. the tracks that will play with no network at all.
	 * A partially cached track — one that was skipped part-way through — is
	 * absent, since offline it would stall where the bytes ran out.
	 */
	val cachedKeys: StateFlow<Set<String>> = _cachedKeys.asStateFlow()

	/** Read on the player's thread when a track starts; see [dataSourceFactory]. */
	@Volatile
	private var writeWhilePlaying = true

	/** Conflated: a burst of span writes is one refresh, not hundreds. */
	private val changes = Channel<Unit>(Channel.CONFLATED)

	init {
		evictor.onChanged = { changes.trySend(Unit) }

		scope.launch {
			settings.cacheMaxBytes.collect { bytes ->
				withContext(Dispatchers.IO) { evictor.setMaxBytes(cache, bytes) }
				refresh()
			}
		}
		scope.launch {
			settings.cacheOnPlay.collect { writeWhilePlaying = it }
		}
		scope.launch {
			refresh()
			while (true) {
				changes.receive()
				// Sleeping before the refresh rather than after is what makes the
				// conflated channel a debounce: everything written during the
				// pause collapses into the single pending signal.
				delay(REFRESH_DEBOUNCE_MS)
				refresh()
			}
		}
	}

	/**
	 * Wraps [upstream] so playback reads from the cache and fills it as it goes.
	 *
	 * A factory of our own rather than a configured [CacheDataSource.Factory]
	 * because the write half is a setting: deciding per created source means
	 * turning caching off takes effect on the next track instead of on the next
	 * app start.
	 */
	fun dataSourceFactory(upstream: DataSource.Factory): DataSource.Factory =
		DataSource.Factory {
			CacheDataSource.Factory()
				.setCache(cache)
				.setUpstreamDataSourceFactory(upstream)
				// A cache that cannot be written must not cost the user their
				// music: fall through to plain streaming instead of failing.
				.setFlags(CacheDataSource.FLAG_IGNORE_CACHE_ON_ERROR)
				.apply { if (!writeWhilePlaying) setCacheWriteDataSinkFactory(null) }
				.createDataSource()
		}

	/**
	 * Whether [key] is held in full.
	 *
	 * The cache's own record of the response length is preferred, because it is
	 * the only figure guaranteed to describe what was actually stored — the
	 * server's song size disagrees the moment a bitrate cap transcodes the
	 * stream. [fallbackLength] is used only when the cache has no length at all,
	 * which for this server is the common case rather than the exception.
	 *
	 * When stream transcoding arrives, this fallback becomes wrong for capped
	 * streams and will need the cap folded into the comparison.
	 */
	fun isFullyCached(key: String, fallbackLength: Long? = null): Boolean {
		val recorded = contentLengthOf(key)
		val length = if (recorded != C.LENGTH_UNSET.toLong()) recorded else fallbackLength
		if (length == null || length <= 0) return false
		return cache.getCachedBytes(key, 0, length) >= length
	}

	private fun contentLengthOf(key: String): Long =
		ContentMetadata.getContentLength(cache.getContentMetadata(key))

	/**
	 * The length the cache recorded for [key], or null when it never learned one.
	 *
	 * Null is a refusal rather than a detail. Without a length there is no
	 * `Content-Length` to send and no way to answer a ranged request, so a caller
	 * serving these bytes over HTTP has to decline and let the fetch go to the
	 * server instead.
	 */
	fun storedLength(key: String): Long? =
		contentLengthOf(key).takeIf { it != C.LENGTH_UNSET.toLong() && it > 0 }

	/**
	 * A read-only source over the stored bytes, for serving a downloaded track to
	 * something that is not ExoPlayer — the Cast bridge, when the track being cast
	 * is already on the device.
	 *
	 * Deliberately given **no upstream factory**: a read that runs off the end of
	 * what is stored then throws rather than quietly going to the network. For a
	 * cast with no connectivity that is the difference between a clear failure and
	 * a stall, which the receiver would eventually report as its own timeout —
	 * error 103, and nothing in the log to say why.
	 */
	fun readOnlySource(): DataSource =
		CacheDataSource.Factory().setCache(cache).createDataSource()

	/**
	 * The quality tag of a copy of [refKey] that is held in full, preferring
	 * [preferred] when that one is present.
	 *
	 * Playing whatever is already stored is the point: after a quality change,
	 * a library downloaded at the old setting is still a library, and re-fetching
	 * it over mobile data to "upgrade" tracks the user can already hear would be
	 * a poor trade. Only an explicit pin refresh re-downloads.
	 *
	 * Null when nothing complete is stored, in which case the caller uses the
	 * current setting and fetches.
	 */
	fun heldTagOf(refKey: String, preferred: AudioQuality): String? {
		val preferredKey = CacheKeys.of(refKey, preferred)
		if (isFullyCached(preferredKey)) return preferred.tag
		return cache.keys
			.firstOrNull { CacheKeys.refKeyOf(it) == refKey && isFullyCached(it) }
			?.let { CacheKeys.tagOf(it) }
	}

	/**
	 * Removes everything unpinned.
	 *
	 * [keepKeys] spares tracks the player is holding — removing a resource that
	 * is being written throws, and taking the floor out from under playback is
	 * not what "free some space" should mean.
	 */
	suspend fun flush(keepKeys: Set<String> = emptySet()) {
		withContext(Dispatchers.IO) {
			cache.keys.forEach { key ->
				// keepKeys arrives as bare refs from the player's queue, while
				// cache keys carry a quality — spare every quality of a track
				// that is in use, not just the one currently set.
				if (pinned.isPinned(key) || CacheKeys.refKeyOf(key) in keepKeys) return@forEach
				// One locked or vanished key must not abort the whole flush.
				runCatching { cache.removeResource(key) }
			}
		}
		refresh()
	}

	private suspend fun refresh() {
		val keys = withContext(Dispatchers.IO) { cache.keys }

		// Fallback for a track the cache recorded no length for: the mirror's
		// own byte size stands in, so a track cached by playing it still counts
		// as stored rather than being dimmed as unavailable while sitting on
		// the device.
		//
		// Only valid for untranscoded bytes. The mirror records the size of the
		// file on the server, which says nothing about the size of an Opus
		// re-encode of it, so anything carrying a quality tag is judged by the
		// cache's own record or not at all.
		val needSize = keys.filter {
			CacheKeys.tagOf(it) == null && contentLengthOf(it) == C.LENGTH_UNSET.toLong()
		}
		val sizes = local.songSizes(needSize)

		val snapshot = withContext(Dispatchers.IO) {
			Snapshot(
				used = cache.cacheSpace,
				pinnedBytes = keys.filter { pinned.isPinned(it) }.sumOf { cachedBytesOf(it) },
				complete = keys.filterTo(mutableSetOf()) { isFullyCached(it, sizes[it]) },
			)
		}
		_usedBytes.value = snapshot.used
		_pinnedBytes.value = snapshot.pinnedBytes
		// Published as bare refs: what the rest of the app asks is "is this
		// track here", not "is it here at this quality". The DAO queries that
		// consume this match `serverId || '/' || id` and would silently return
		// nothing if a quality suffix reached them.
		_cachedKeys.value = snapshot.complete
			.mapTo(mutableSetOf()) { CacheKeys.refKeyOf(it) }
	}

	private fun cachedBytesOf(key: String): Long =
		cache.getCachedSpans(key).sumOf { it.length }

	private data class Snapshot(
		val used: Long,
		val pinnedBytes: Long,
		val complete: Set<String>,
	)

	private companion object {
		/** Long enough to coalesce a track's worth of span writes. */
		const val REFRESH_DEBOUNCE_MS = 500L
	}
}
