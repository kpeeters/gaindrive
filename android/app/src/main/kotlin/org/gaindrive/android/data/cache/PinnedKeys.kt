package org.gaindrive.android.data.cache

import javax.inject.Inject
import javax.inject.Singleton

/**
 * The cache keys that eviction must never touch.
 *
 * A plain in-memory set rather than a query: [PinAwareEvictor] is called back on
 * cache writer threads, inside the cache's own lock, where it can neither
 * suspend nor touch a database. Whoever owns the pins keeps this in step -
 * see `PinRepository` from the downloads stage.
 *
 * Empty until pinning exists, which makes eviction plain LRU.
 */
@Singleton
class PinnedKeys @Inject constructor() {

	@Volatile
	var keys: Set<String> = emptySet()

	fun isPinned(key: String): Boolean = keys.contains(key)
}
