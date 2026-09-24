package org.gaindrive.android.data

import java.net.URLDecoder

/**
 * Reading a `gaindrive://` track link, and deciding which configured server it
 * belongs to.
 *
 * Pure string work, no Android types, for the reason [extractSharedUrl] gives:
 * it can then be tested without a device. The link arrives as the data URI of
 * a VIEW intent - `gaindrive://<host>[:port]<path>?track=<id>[&t=<seconds>]` -
 * composed by the server's chooser page from the URL the person actually
 * opened, so the authority and path are the server's public spelling of
 * itself, proxy subpath included.
 */
data class TrackLink(
	/** `host[:port]`, exactly as the link spelled it. */
	val authority: String,
	/** The path before the query, `/` included; where the server is mounted. */
	val path: String,
	/** The Subsonic song id, undecoded meaning: the server's own spelling. */
	val trackId: String,
	/** Start position, 0 when the link named none. */
	val positionMs: Long,
)

/**
 * The [TrackLink] in [uri], or null when it is not one - the wrong scheme, no
 * authority, or no usable `track` parameter. Null means "not for us", so the
 * intent is ignored rather than answered with an error: this activity's VIEW
 * filter admits only the scheme, but an intent is what another app says it is.
 */
fun parseTrackLink(uri: String?): TrackLink? {
	if (uri == null) return null
	val scheme = "gaindrive://"
	if (!uri.regionMatches(0, scheme, 0, scheme.length, ignoreCase = true)) return null

	val rest = uri.substring(scheme.length)
	val authorityEnd = rest.indexOfFirst { it == '/' || it == '?' || it == '#' }
		.let { if (it == -1) rest.length else it }
	val authority = rest.take(authorityEnd)
	if (authority.isEmpty()) return null

	val afterAuthority = rest.substring(authorityEnd).substringBefore('#')
	val path = afterAuthority.substringBefore('?')
	val query = if ('?' in afterAuthority) afterAuthority.substringAfter('?') else ""

	var trackId: String? = null
	var seconds = 0.0
	for (pair in query.split('&')) {
		if (pair.isEmpty()) continue
		val key = pair.substringBefore('=')
		// A share URL is percent-encoded by whoever composed it; ids are
		// integers today, but the server's spelling is the one that matters
		// and decoding is what hands it back.
		val value = try {
			URLDecoder.decode(pair.substringAfter('=', ""), "UTF-8")
		} catch (_: IllegalArgumentException) {
			continue // a malformed %-escape spoils that value, not the link
		}
		when (key) {
			"track" -> if (value.isNotEmpty()) trackId = value
			"t" -> seconds = value.toDoubleOrNull()?.takeIf { it > 0 } ?: seconds
		}
	}

	return trackId?.let {
		TrackLink(authority, path, it, (seconds * 1000).toLong())
	}
}

/**
 * Whether the server configured at [configUrl] is the one [link] names.
 *
 * The authority is compared case-insensitively - a DNS name is - and the
 * link's path must sit at or under the configured one, so two instances
 * mounted under different subpaths of one host stay distinct. The scheme is
 * deliberately not part of it: the link travelled under the app's own scheme
 * and lost the original, and the configured URL already knows which one it
 * speaks.
 *
 * An explicit default port (`:443`, `:80`) on one side and not the other is a
 * miss. Accepted: neither the server's chooser page nor a person types one.
 */
fun serverMatchesLink(configUrl: String, link: TrackLink): Boolean {
	val afterScheme = configUrl.substringAfter("://", "")
	if (afterScheme.isEmpty()) return false
	val slash = afterScheme.indexOf('/')
	val configAuthority = if (slash == -1) afterScheme else afterScheme.take(slash)
	if (!configAuthority.equals(link.authority, ignoreCase = true)) return false

	// "" for a root mount either way round; trailing slashes are spelling.
	val configPath = (if (slash == -1) "" else afterScheme.substring(slash)).trimEnd('/')
	val linkPath = link.path.trimEnd('/')
	return linkPath == configPath || linkPath.startsWith("$configPath/")
}
