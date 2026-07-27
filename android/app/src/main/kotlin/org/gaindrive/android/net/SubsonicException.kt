package org.gaindrive.android.net

import java.io.IOException

/**
 * A non-`ok` Subsonic status. Extends IOException so it travels the same path
 * as a transport failure and no call site has to catch two families.
 *
 * No code takes the whole app out of service: with several servers configured,
 * one bad account is a per-server condition.
 */
class SubsonicException(
	val code: Int,
	override val message: String,
) : IOException(message) {

	/** Credentials are wrong or the account is disabled. */
	val isAuthFailure: Boolean get() = code == WRONG_CREDENTIALS

	/** Authenticated, but this account may not do that. */
	val isForbidden: Boolean get() = code == NOT_AUTHORISED

	companion object {
		const val MISSING_PARAMETER = 10
		const val WRONG_CREDENTIALS = 40
		const val NOT_AUTHORISED = 50
		const val NOT_FOUND = 70
	}
}

/** Throws on a non-`ok` status, otherwise returns the body. */
fun <T : SubsonicBody> SubsonicEnvelope<T>.requireOk(): T {
	val body = response
	if (body.status == "ok") return body
	val error = body.error
	throw SubsonicException(
		code = error?.code ?: 0,
		message = error?.message?.takeIf { it.isNotBlank() } ?: "Unknown server error",
	)
}
