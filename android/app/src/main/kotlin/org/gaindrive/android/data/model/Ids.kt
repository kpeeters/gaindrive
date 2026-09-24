package org.gaindrive.android.data.model

import kotlinx.serialization.Serializable
import java.util.UUID

/**
 * Identifies a configured server. Generated locally when the server is added,
 * never derived from its URL - a server that moves from a LAN address to a
 * domain name is still the same server, and its ids, starred items and queue
 * references have to survive the move.
 */
@JvmInline
@Serializable
value class ServerId(val value: String) {
	override fun toString(): String = value

	companion object {
		fun new(): ServerId = ServerId(UUID.randomUUID().toString())
	}
}

/**
 * A Subsonic id together with the server that issued it. Subsonic ids are only
 * meaningful relative to their server - two servers will both have an artist
 * with id 42 - so no bare id may cross a layer boundary.
 */
data class ItemRef(val server: ServerId, val id: String) {

	/**
	 * Encoded for `MediaItem.mediaId`, which is the only context Media3 hands
	 * back on notification actions and session restore.
	 */
	fun encode(): String = "${server.value}/$id"

	companion object {
		/**
		 * Several refs in one navigation argument, for a row that stands for the
		 * same artist on more than one server. Neither half of a ref can contain
		 * a comma, so the split is unambiguous.
		 */
		fun encodeAll(refs: List<ItemRef>): String = refs.joinToString(",") { it.encode() }

		/** Inverse of [encodeAll]; malformed entries are dropped, not fatal. */
		fun decodeAll(encoded: String): List<ItemRef> =
			encoded.split(',').mapNotNull { decode(it) }

		/** Inverse of [encode]; null when the string is not a valid ref. */
		fun decode(encoded: String): ItemRef? {
			// Server ids are UUIDs and Subsonic ids are integers, so neither
			// half can contain the separator. Splitting on the first '/' is
			// therefore unambiguous.
			val cut = encoded.indexOf('/')
			if (cut <= 0 || cut == encoded.length - 1) return null
			return ItemRef(
				ServerId(encoded.substring(0, cut)),
				encoded.substring(cut + 1),
			)
		}
	}
}
