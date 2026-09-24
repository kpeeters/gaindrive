package org.gaindrive.android.data.cache

/**
 * What the download manager is doing, as far as anything outside needs to know.
 *
 * Deliberately free of Media3 types: the rules that read this - `pinPhaseOf`,
 * the row indicators - are then testable without an Android runtime, and the
 * mapping from `Download.STATE_*` lives in exactly one place.
 */
data class DownloadStates(
	/** Cache key to progress, for downloads not in a terminal state. */
	val active: Map<String, TrackDownload> = emptyMap(),
	/**
	 * Keys the download manager considers fully downloaded.
	 *
	 * Load-bearing, not a convenience. `AudioCache.isFullyCached` can only judge
	 * a track when the cache recorded a content length, and gaindrive's
	 * `stream.view` answers through a content provider - so cpp-httplib sends it
	 * chunked, with no `Content-Length`, and the length stays unset for ever.
	 * Without this, a download that finished perfectly well never counted as
	 * stored: the progress ring sat at "0 of 9" and offline dimming would have
	 * greyed out music that was right there on the device.
	 */
	val completed: Set<String> = emptySet(),
	val failed: Set<String> = emptySet(),
	/**
	 * Requirement flags that are *not* currently met - non-zero means every
	 * queued download is waiting rather than progressing. Usually the Wi-Fi-only
	 * setting on a metered connection.
	 */
	val notMetRequirements: Int = 0,
)

/**
 * Two states, not Media3's seven. A track is either being fetched right now or
 * it is waiting its turn, and that is the whole of what a row can usefully
 * show in the space of a tick.
 */
enum class TrackDownloadState { QUEUED, DOWNLOADING }

data class TrackDownload(
	val state: TrackDownloadState,
	/** Negative when the manager has no figure yet; see [knownFraction]. */
	val percent: Float,
) {
	val queued: Boolean get() = state == TrackDownloadState.QUEUED

	/**
	 * Null until there is a real figure, which is what the row needs to decide
	 * between an honest arc and a spinner. A determinate ring pinned at zero
	 * because nothing is known yet looks stalled rather than starting.
	 */
	val knownFraction: Float?
		get() = if (state == TrackDownloadState.DOWNLOADING && percent >= 0f) {
			(percent / 100f).coerceIn(0f, 1f)
		} else {
			null
		}
}
