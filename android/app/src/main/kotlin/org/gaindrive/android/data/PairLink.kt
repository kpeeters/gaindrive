package org.gaindrive.android.data

import java.net.URLDecoder
import java.util.Base64
import kotlinx.serialization.Serializable

/**
 * Reading and writing a `gaindrive://pair` link, the QR a TV shows so a phone
 * can send its server logins over.
 *
 * Pure string work, no Android types, for [parseTrackLink]'s reason: the
 * parser is the trust boundary - the VIEW filter admits anything under the
 * scheme - and this way it is tested without a device. The TV composes the
 * link with [buildPairUri]; the phone's camera app hands it back as the data
 * URI of a VIEW intent.
 */
class PairLink(
	/** Dotted IPv4 of the TV's listener, exactly as the link spelled it. */
	val host: String,
	val port: Int,
	/** The listener's session token, required as the request path. */
	val token: String,
	/** The AES key the payload must be encrypted with. */
	val key: ByteArray,
)

/**
 * What actually crosses the LAN, AES-GCM encrypted with the QR's key. Ids are
 * deliberately absent: the TV mints its own. Every field defaulted, the
 * [StoredServer] discipline, so the two ends can grow fields independently.
 */
@Serializable
data class PairPayload(val servers: List<PairServer> = emptyList())

@Serializable
data class PairServer(
	val name: String = "",
	val url: String = "",
	val username: String = "",
	val password: String = "",
	val browseByFolder: Boolean = false,
)

/** The AES key length [parsePairLink] insists on, in bytes. */
const val PAIR_KEY_BYTES = 32

/**
 * The `gaindrive://pair?host=…&port=…&t=…&k=…` URI for a listener. The key
 * travels base64url without padding: the query values are percent-decoded on
 * the way back in, and plain base64's `+` would decode to a space there.
 */
fun buildPairUri(host: String, port: Int, token: String, key: ByteArray): String {
	val k = Base64.getUrlEncoder().withoutPadding().encodeToString(key)
	return "gaindrive://pair?host=$host&port=$port&t=$token&k=$k"
}

/**
 * The [PairLink] in [uri], or null when it is not one - the wrong scheme or
 * authority, a port outside the range, a key of the wrong size. Null means
 * "not for us" and the intent is ignored, exactly as [parseTrackLink] treats
 * what it does not recognise; the two parsers split the one scheme between
 * them by authority.
 */
fun parsePairLink(uri: String?): PairLink? {
	if (uri == null) return null
	val scheme = "gaindrive://"
	if (!uri.regionMatches(0, scheme, 0, scheme.length, ignoreCase = true)) return null

	val rest = uri.substring(scheme.length)
	val authorityEnd = rest.indexOfFirst { it == '/' || it == '?' || it == '#' }
		.let { if (it == -1) rest.length else it }
	if (!rest.take(authorityEnd).equals("pair", ignoreCase = true)) return null

	val afterAuthority = rest.substring(authorityEnd).substringBefore('#')
	val query = if ('?' in afterAuthority) afterAuthority.substringAfter('?') else ""

	var host: String? = null
	var port = 0
	var token: String? = null
	var key: ByteArray? = null
	for (pair in query.split('&')) {
		if (pair.isEmpty()) continue
		val value = try {
			URLDecoder.decode(pair.substringAfter('=', ""), "UTF-8")
		} catch (_: IllegalArgumentException) {
			continue // a malformed %-escape spoils that value, not the link
		}
		when (pair.substringBefore('=')) {
			// A host with a slash could steer the POST somewhere the QR never
			// said; the port is in its own parameter, so a colon is as foreign.
			"host" -> if (value.isNotEmpty() && !value.any { it == '/' || it == ':' }) {
				host = value
			}
			"port" -> port = value.toIntOrNull()?.takeIf { it in 1..65535 } ?: 0
			"t" -> if (value.isNotEmpty()) token = value
			"k" -> key = try {
				Base64.getUrlDecoder().decode(value).takeIf { it.size == PAIR_KEY_BYTES }
			} catch (_: IllegalArgumentException) {
				null
			}
		}
	}

	val h = host ?: return null
	val t = token ?: return null
	val k = key ?: return null
	if (port == 0) return null
	return PairLink(h, port, t, k)
}
