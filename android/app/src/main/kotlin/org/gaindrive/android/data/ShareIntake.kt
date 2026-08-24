package org.gaindrive.android.data

/**
 * Pulling the URL out of text another app shared with us.
 *
 * Pure string work, no Android types, so it can be tested without a device —
 * the same bargain `src/videoname.hh` strikes on the server for filename
 * parsing.
 *
 * It exists because `Intent.EXTRA_TEXT` is very often *not* a bare URL. YouTube
 * shares the video's title and then its link on the next line; a browser may
 * send a title, a link and a line of its own; a person quoting something sends
 * whatever they typed. The server refuses anything that is not http(s), so
 * handing the shared text straight to `fetchUrl` fails on the commonest share
 * this feature exists for.
 */

/**
 * The first http(s) URL in [shared], or null if there is none.
 *
 * The first rather than the longest or the last: a share that carries several
 * links leads with the one it is about, and the alternatives are guesses that
 * are wrong just as often. The user sees what was picked before anything is
 * fetched.
 *
 * Trailing punctuation is stripped, because a link at the end of a sentence
 * arrives with the full stop attached. Brackets are only stripped when
 * unbalanced — a closing one that has an opener inside the URL belongs to it,
 * which is how Wikipedia's `..._(disambiguation)` survives.
 */
fun extractSharedUrl(shared: String?): String? {
	if (shared.isNullOrBlank()) return null
	val match = URL_PATTERN.find(shared) ?: return null
	// A bare scheme is not a URL. Trimming punctuation can also produce one, so
	// this is checked after the trim rather than left to the pattern.
	return trimTrailing(match.value).takeIf { it.substringAfter("//").isNotEmpty() }
}

/**
 * Deliberately not anchored and deliberately not a validator. Its job is to
 * *find* a candidate in prose; whether the server will accept it is the
 * server's answer to give, and it is the one that matters.
 */
private val URL_PATTERN = Regex("""https?://\S+""", RegexOption.IGNORE_CASE)

private fun trimTrailing(url: String): String {
	var end = url.length
	while (end > 0) {
		val c = url[end - 1]
		val drop = when (c) {
			'.', ',', ';', ':', '!', '?', '"', '\'', '>' -> true
			')' -> url.take(end).count { it == '(' } < url.take(end).count { it == ')' }
			']' -> url.take(end).count { it == '[' } < url.take(end).count { it == ']' }
			else -> false
		}
		if (!drop) break
		end--
	}
	return url.take(end)
}
