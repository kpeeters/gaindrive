package org.gaindrive.android.data

import org.gaindrive.android.data.model.Chapter
import java.net.URLEncoder
import kotlin.math.floor

/**
 * Composing a track link to share, the inverse of [parseTrackLink]'s intake.
 *
 * Pure string work, no Android types, the [extractSharedUrl] bargain again.
 * The shape is the web client's exactly — `<server>/?track=<id>[&t=<s>]` — so
 * a link made here lands in the same chooser page, web player and app intake
 * as one copied from a browser.
 */

/**
 * The link for [trackId] on the server at [serverUrl] (stored form, no
 * trailing slash), starting [tSeconds] in. Zero or negative means the plain
 * link — `t=0` is rightly no parameter at all.
 */
fun trackShareUrl(serverUrl: String, trackId: String, tSeconds: Double = 0.0): String {
	// Encoded as the web's encodeURIComponent does; ids are integers today,
	// but this side must not be the one that breaks first if that changes.
	val base = "$serverUrl/?track=${URLEncoder.encode(trackId, "UTF-8")}"
	return if (tSeconds > 0) "$base&t=${formatT(tSeconds)}" else base
}

/**
 * `t` as the web emits it: an integer when whole ("90", never "90.0"), the
 * fraction verbatim otherwise ("6491.238") — both readers parse it as a float,
 * and a chapter's start keeps its millisecond precision through a round trip.
 */
private fun formatT(t: Double): String =
	if (t == floor(t)) t.toLong().toString() else t.toString()

/**
 * The chapter under [positionSeconds]: the last marker at or before it, both
 * sides in seconds — the unit `Chapter.startSeconds` documents, and the one
 * the web dialog first got wrong. Null when there is none, and null for a
 * marker starting at 0 — "start at this chapter" would be the plain link, the
 * same "offers nothing" rule the position checkbox applies to 0:00.
 */
fun chapterAt(chapters: List<Chapter>, positionSeconds: Long): Chapter? =
	chapters.lastOrNull { it.startSeconds <= positionSeconds }
		?.takeIf { it.startSeconds > 0 }
