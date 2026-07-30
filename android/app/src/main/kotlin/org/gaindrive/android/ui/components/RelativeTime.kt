package org.gaindrive.android.ui.components

import android.text.format.DateUtils
import java.time.LocalDateTime
import java.time.OffsetDateTime
import java.time.ZoneOffset

/**
 * "3 hours ago" for a server timestamp, or null when it cannot be read.
 *
 * Two formats are accepted on purpose. gaindrive writes `last_played` with
 * SQLite's `CURRENT_TIMESTAMP`, which is UTC in `yyyy-MM-dd HH:mm:ss` — no `T`,
 * no zone — while other Subsonic servers send ISO-8601. Parsing only one of
 * them would leave the column silently blank against the other.
 */
fun relativeTime(timestamp: String?): String? {
	val millis = epochMillisOf(timestamp ?: return null) ?: return null
	return DateUtils.getRelativeTimeSpanString(
		millis,
		System.currentTimeMillis(),
		DateUtils.MINUTE_IN_MILLIS,
	).toString()
}

private fun epochMillisOf(timestamp: String): Long? {
	val text = timestamp.trim().ifBlank { return null }
	return runCatching { OffsetDateTime.parse(text).toInstant().toEpochMilli() }.getOrNull()
		?: runCatching {
			LocalDateTime.parse(text.replace(' ', 'T'))
				.toInstant(ZoneOffset.UTC)
				.toEpochMilli()
		}.getOrNull()
}
