package org.gaindrive.android.data.cache

import org.gaindrive.android.data.model.ItemRef

/** What a pin was placed on. Only [PinKind.SONG] names a cache key directly. */
enum class PinKind { SONG, ALBUM, PLAYLIST }

data class Pin(val ref: ItemRef, val kind: PinKind)

/** A pin with a name on it, for anywhere that lists them back to the user. */
data class PinnedItem(val pin: Pin, val label: String)

/**
 * What each pin covers: its own encoded ref, to the song cache keys under it.
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

/** How far a pin has got, for the icon that reports it. */
data class PinStatus(val stored: Int, val total: Int, val active: Boolean) {

	/**
	 * A pin covering nothing counts as done rather than forever in progress: a
	 * permanent spinner over an album whose track list has since been dropped
	 * would be a bug the user could do nothing about.
	 */
	val complete: Boolean get() = stored >= total

	val fraction: Float get() = if (total <= 0) 1f else stored.toFloat() / total
}
