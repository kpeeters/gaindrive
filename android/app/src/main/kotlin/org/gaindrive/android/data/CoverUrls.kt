package org.gaindrive.android.data

import org.gaindrive.android.data.model.ItemRef
import org.gaindrive.android.data.model.ServerId
import org.gaindrive.android.net.SubsonicClient

/**
 * Turns cover-art references into URLs Coil can load.
 *
 * Built once per screen load rather than per item: resolving a server means
 * reading DataStore and decrypting a password, which is far too expensive to
 * repeat for every row in a list.
 *
 * Keyed by server so a merged list works unchanged - each row's URL comes from
 * the server that owns it.
 */
class CoverUrls internal constructor(
	private val clients: Map<ServerId, SubsonicClient>,
) {

	/**
	 * [size] is the pixel size the server should scale to. Ask for what is
	 * actually displayed: `ArtKeys` strips the auth off this before Coil sees
	 * it but keeps the size, because the server serves a different ladder rung
	 * per size, so a consistent size per context is what makes the cache hit.
	 */
	fun url(ref: ItemRef?, size: Int): String? {
		val client = clients[ref?.server] ?: return null
		return client.url("getCoverArt", mapOf("id" to ref!!.id, "size" to size.toString()))
	}
}
