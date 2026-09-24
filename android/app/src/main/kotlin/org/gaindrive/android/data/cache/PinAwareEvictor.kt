package org.gaindrive.android.data.cache

import androidx.media3.common.C
import androidx.media3.datasource.cache.Cache
import androidx.media3.datasource.cache.CacheEvictor
import androidx.media3.datasource.cache.CacheSpan
import java.util.TreeSet

/**
 * Least-recently-used eviction that never touches pinned content.
 *
 * Media3's own `LeastRecentlyUsedCacheEvictor` would drop a pinned album the
 * moment enough other music had been played, which is exactly the failure the
 * user asked to avoid: the automatic answer always deletes the thing you were
 * about to want. So the LRU order is kept as usual and pinned keys are simply
 * skipped when picking a victim.
 *
 * [maxBytes] is mutable because the cap is a setting. Rebuilding the cache to
 * change it would mean releasing the player mid-session, which is a lot of
 * upheaval for moving a slider.
 *
 * Every method is synchronized: the cache calls in from its writer threads,
 * while [setMaxBytes] arrives from whoever changed the setting.
 */
class PinAwareEvictor(
	initialMaxBytes: Long,
	private val pinned: PinnedKeys,
) : CacheEvictor {

	/** Ordered oldest-touch first, which is the order victims are picked in. */
	private val leastRecentlyUsed = TreeSet<CacheSpan>(
		Comparator { a, b ->
			val delta = a.lastTouchTimestamp - b.lastTouchTimestamp
			// Falls back to the span's own ordering so two spans touched in the
			// same millisecond are still distinct - a TreeSet drops "equal"
			// entries, and a dropped span would leak from the size accounting.
			when {
				delta < 0 -> -1
				delta > 0 -> 1
				else -> a.compareTo(b)
			}
		}
	)

	private var currentSize = 0L
	private var maxBytes = initialMaxBytes

	/**
	 * Notified after any change to what is cached, so the usage readout and the
	 * "is this track available offline" index can catch up. Called with the
	 * cache lock held, so it must not block - post and return.
	 */
	@Volatile
	var onChanged: (() -> Unit)? = null

	/**
	 * Takes the cache's own monitor before ours, deliberately.
	 *
	 * That is the order a cache writer thread arrives in - cache lock held,
	 * then a callback into here - and taking the two in the opposite order from
	 * this side would eventually deadlock the player against a settings change.
	 */
	fun setMaxBytes(cache: Cache, bytes: Long) {
		synchronized(cache) {
			synchronized(this) {
				maxBytes = bytes
				evict(cache, 0)
			}
		}
	}

	override fun requiresCacheSpanTouches(): Boolean = true

	override fun onCacheInitialized() = Unit

	@Synchronized
	override fun onStartFile(cache: Cache, key: String, position: Long, length: Long) {
		// An unknown length cannot be made room for; the eviction on the spans
		// that do arrive will catch up.
		if (length != C.LENGTH_UNSET.toLong()) evict(cache, length)
	}

	@Synchronized
	override fun onSpanAdded(cache: Cache, span: CacheSpan) {
		leastRecentlyUsed.add(span)
		currentSize += span.length
		evict(cache, 0)
		onChanged?.invoke()
	}

	@Synchronized
	override fun onSpanRemoved(cache: Cache, span: CacheSpan) {
		leastRecentlyUsed.remove(span)
		currentSize -= span.length
		onChanged?.invoke()
	}

	@Synchronized
	override fun onSpanTouched(cache: Cache, oldSpan: CacheSpan, newSpan: CacheSpan) {
		onSpanRemoved(cache, oldSpan)
		onSpanAdded(cache, newSpan)
	}

	/**
	 * Frees space until the cap holds, or until only pinned spans are left.
	 *
	 * Running out of unpinned victims is a real state, not an error: pinning
	 * more than the cap allows leaves the cache legitimately over it. The pin
	 * action is where that is refused, because there it can be explained.
	 */
	private fun evict(cache: Cache, requiredSpace: Long) {
		while (currentSize + requiredSpace > maxBytes) {
			// Found before removing, not during: removeSpan calls straight back
			// into onSpanRemoved and mutates the set we would be iterating.
			val victim = leastRecentlyUsed.firstOrNull { !pinned.isPinned(it.key) } ?: return
			cache.removeSpan(victim)
		}
	}
}
