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

	fun isFullyCached(key: String): Boolean {
		val length = ContentMetadata.getContentLength(cache.getContentMetadata(key))
		// Length comes from the cache's own record of the response rather than
		// from the server's reported song size: the two differ as soon as a
		// bitrate cap transcodes the stream, and only the former is comparable
		// with what was actually stored.
		if (length == C.LENGTH_UNSET.toLong()) return false
		return cache.getCachedBytes(key, 0, length) >= length
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
				if (pinned.isPinned(key) || key in keepKeys) return@forEach
				// One locked or vanished key must not abort the whole flush.
				runCatching { cache.removeResource(key) }
			}
		}
		refresh()
	}

	private suspend fun refresh() {
		val snapshot = withContext(Dispatchers.IO) {
			val keys = cache.keys
			Snapshot(
				used = cache.cacheSpace,
				pinnedBytes = keys.filter { pinned.isPinned(it) }.sumOf { cachedBytesOf(it) },
				complete = keys.filterTo(mutableSetOf()) { isFullyCached(it) },
			)
		}
		_usedBytes.value = snapshot.used
		_pinnedBytes.value = snapshot.pinnedBytes
		_cachedKeys.value = snapshot.complete
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
