package org.gaindrive.android.net

import kotlinx.serialization.json.Json

/**
 * How every Subsonic response is parsed.
 *
 * Defined here rather than in the Hilt module so the tests can use the real
 * thing. A test that builds its own configuration proves only that the DTOs
 * match some parser, which is how a server whose ids are unquoted got as far as
 * a user's phone.
 *
 * All three settings exist because the payload comes from someone else's server
 * and this app is a reader, not a validator. Nothing is made safer by refusing
 * a response — the user just loses their library.
 */
val SubsonicJson: Json = Json {
	// Servers add fields over time and OpenSubsonic extensions add more; an
	// unknown key must never fail a response.
	ignoreUnknownKeys = true
	// Tolerates nulls where the DTO declares a non-null default, which some
	// Subsonic implementations emit for absent values.
	coerceInputValues = true
	// Ids are declared String throughout, because that is what they are to this
	// app: opaque tokens handed back as query parameters. Subsonic's own XSD
	// types several of them as integers, though, and servers predating the JSON
	// API emit them unquoted — `{"id":1}` rather than `{"id":"1"}`. Without
	// this, one such field fails the whole response, which is how enabling a
	// legacy server broke the artist list with "Expected quotation mark".
	//
	// Lenient parsing also reads a quoted number into an Int field, covering
	// the mirror-image case a different server will eventually present.
	isLenient = true
}
