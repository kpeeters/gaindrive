package org.gaindrive.android.data.cache

import org.gaindrive.android.data.model.AudioQuality
import org.gaindrive.android.data.model.ItemRef

/**
 * Cache keys, which are an encoded [ItemRef] plus the quality that was
 * **asked for**: `<serverId>/<songId>@opus160`.
 *
 * The quality has to be part of the key because the same track can be held at
 * more than one quality — changing the setting re-downloads pins, and the old
 * copies stay playable until eviction reclaims them.
 *
 * It names the request rather than the response, and that distinction became
 * real when the app began declaring what it takes as it stands: an `@opus160`
 * key can hold the original MP3. The key is still exactly as safe, because what
 * it has to keep apart is two requests that would store different bytes, and
 * every request for one track at one quality declares the same thing. What no
 * longer follows from it is the *format* — read that off the bytes with
 * `AudioCache.storedMimeType`, or off the decoder with
 * `PlayerState.deliveredMime`, and never off the tag.
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
