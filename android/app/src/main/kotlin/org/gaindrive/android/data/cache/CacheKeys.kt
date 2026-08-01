package org.gaindrive.android.data.cache

import org.gaindrive.android.data.model.AudioQuality
import org.gaindrive.android.data.model.ItemRef

/**
 * Cache keys, which are an encoded [ItemRef] plus the quality of the bytes
 * stored under it: `<serverId>/<songId>@opus160`.
 *
 * The quality has to be part of the key because the same track can be held at
 * more than one quality — changing the setting re-downloads pins, and the old
 * copies stay playable until eviction reclaims them.
 *
 * ## Where a suffixed key may appear
 *
 * Exactly three places:
 *
 *  - `MediaItem.customCacheKey`
 *  - `DownloadRequest.customCacheKey` (**not** its `id`, which stays a bare ref)
 *  - `PinnedKeys` / `PinAwareEvictor`
 *
 * Everywhere else uses the bare `ref.encode()`: `PinCoverage`, all of
 * `DownloadStates`, `AudioCache.cachedKeys`, `StoredFilter`, `AvailabilityState`
 * and — most importantly — the `LibraryDao` queries that match
 * `serverId || '/' || id`.
 *
 * Those queries **fail silently** if a suffix reaches them: SQL matches nothing,
 * so the result is an empty list rather than an exception. The symptom is
 * offline availability marks quietly disappearing with nothing in the log.
 * [refKeyOf] is the only way back, and `AudioCache` is the only place that
 * should need to call it.
 */
object CacheKeys {

	/**
	 * A server id is a UUID and a Subsonic id is an integer, so neither can
	 * contain this — the same argument [ItemRef.decode] makes for '/'.
	 */
	private const val SEP = '@'

	fun of(ref: ItemRef, quality: AudioQuality): String =
		"${ref.encode()}$SEP${quality.tag}"

	fun of(refKey: String, quality: AudioQuality): String =
		"$refKey$SEP${quality.tag}"

	/** The bare `ItemRef.encode()` half. Safe on an unsuffixed key. */
	fun refKeyOf(cacheKey: String): String = cacheKey.substringBefore(SEP)

	/** The quality half, or null for a key written before quality existed. */
	fun tagOf(cacheKey: String): String? =
		cacheKey.substringAfter(SEP, "").ifEmpty { null }
}
