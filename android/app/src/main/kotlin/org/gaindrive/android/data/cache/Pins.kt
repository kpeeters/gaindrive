package org.gaindrive.android.data.cache

import org.gaindrive.android.data.model.ItemRef

/** What a pin was placed on. Only [PinKind.SONG] names a cache key directly. */
enum class PinKind { SONG, ALBUM, PLAYLIST }

data class Pin(val ref: ItemRef, val kind: PinKind)

/** A pin with a name on it, for anywhere that lists them back to the user. */
data class PinnedItem(val pin: Pin, val label: String)

/**
 * What each pin covers: its own encoded ref, to the encoded refs of the songs
 * under it.
 *
 * Bare refs on both sides, not cache keys — quality belongs to the bytes, not
 * to what a pin covers, and the same pin protects whichever quality is set at
 * the time. `PinRepository.applyProtection` is where the union is turned into
 * cache keys for the evictor. See `CacheKeys` for why the distinction matters.
 *
 * Kept per pin rather than flattened, because the two questions want different
 * shapes — eviction needs the union ([allKeys]), while the download indicator
 * needs to know how much of *this* album has arrived.
 */
@JvmInline
value class PinCoverage(val byPin: Map<String, List<String>>) {
	val allKeys: Set<String> get() = byPin.values.flatMapTo(mutableSetOf()) { it }
}

/**
 * The audio cache keys a set of pins covers.
 *
 * Pure, and takes membership already resolved, so the rule that decides what
 * survives eviction can be tested without a database or a cache. Membership is
 * passed in rather than looked up because it changes underneath a pin — a
 * playlist gains a track and the pin has to cover it.
 *
 * Unknown members are simply absent: pinning an album that has never been
 * opened protects nothing until its track list has been seen, which is the
 * honest answer rather than a guess.
 */
fun expandPins(
	pins: List<Pin>,
	albumSongs: Map<ItemRef, List<ItemRef>>,
	playlistSongs: Map<ItemRef, List<ItemRef>>,
): PinCoverage = PinCoverage(
	pins.associate { pin ->
		val songs = when (pin.kind) {
			PinKind.SONG -> listOf(pin.ref)
			PinKind.ALBUM -> albumSongs[pin.ref].orEmpty()
			PinKind.PLAYLIST -> playlistSongs[pin.ref].orEmpty()
		}
		pin.ref.encode() to songs.map { it.encode() }
	}
)

/** What a pin is doing, as far as the icon reporting it is concerned. */
enum class PinPhase {
	/** Every track it covers is stored. */
	COMPLETE,

	/** Downloading, or just asked for and not yet reported on. */
	RUNNING,

	/** Queued, but a requirement is unmet — in practice, waiting for Wi-Fi. */
	WAITING,

	/** At least one track failed and nothing is retrying it. */
	FAILED,
}

/** How far a pin has got, for the icon that reports it. */
data class PinStatus(
	val stored: Int,
	val total: Int,
	val phase: PinPhase,
) {
	val complete: Boolean get() = phase == PinPhase.COMPLETE

	val fraction: Float get() = if (total <= 0) 1f else stored.toFloat() / total
}

/**
 * Which of the four a pin is in.
 *
 * Order matters. Completion wins outright. A failure only shows once nothing is
 * still trying, so one track failing part-way through an album does not stop the
 * rest reporting progress. "Waiting" needs something actually queued behind it,
 * or an album that finished would report itself as waiting the moment the
 * connection went metered.
 *
 * Anything else is [PinPhase.RUNNING], including the moment just after a tap
 * when the download manager has not reported anything yet — showing failure
 * there would flash an error on every single pin.
 */
fun pinPhaseOf(
	coveredKeys: List<String>,
	storedKeys: Set<String>,
	downloads: DownloadStates,
): PinPhase {
	val stored = coveredKeys.count { it in storedKeys }
	val queued = coveredKeys.count { it in downloads.active }
	return when {
		stored >= coveredKeys.size -> PinPhase.COMPLETE
		queued == 0 && coveredKeys.any { it in downloads.failed } -> PinPhase.FAILED
		queued > 0 && downloads.notMetRequirements != 0 -> PinPhase.WAITING
		else -> PinPhase.RUNNING
	}
}
