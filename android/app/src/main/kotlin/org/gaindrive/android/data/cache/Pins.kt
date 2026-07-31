package org.gaindrive.android.data.cache

import org.gaindrive.android.data.model.ItemRef

/** What a pin was placed on. Only [PinKind.SONG] names a cache key directly. */
enum class PinKind { SONG, ALBUM, PLAYLIST }

data class Pin(val ref: ItemRef, val kind: PinKind)

/** A pin with a name on it, for anywhere that lists them back to the user. */
data class PinnedItem(val pin: Pin, val label: String)

/**
 * The audio cache keys a set of pins protects.
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
): Set<String> = buildSet {
	pins.forEach { pin ->
		when (pin.kind) {
			PinKind.SONG -> add(pin.ref.encode())
			PinKind.ALBUM -> albumSongs[pin.ref]?.forEach { add(it.encode()) }
			PinKind.PLAYLIST -> playlistSongs[pin.ref]?.forEach { add(it.encode()) }
		}
	}
}
