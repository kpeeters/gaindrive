package org.gaindrive.android.net

import kotlinx.serialization.SerializationException
import retrofit2.HttpException
import java.io.IOException
import java.net.ConnectException
import java.net.SocketTimeoutException
import java.net.UnknownHostException
import javax.net.ssl.SSLException
import kotlin.coroutines.cancellation.CancellationException

/**
 * Like [runCatching], but never swallows cancellation.
 *
 * `runCatching` catches `Throwable`, which includes the `CancellationException`
 * coroutines use to unwind a cancelled job. Swallowing it turns "the user
 * navigated away" into "the screen shows an error", and leaves the coroutine
 * machinery believing the job completed normally.
 */
inline fun <T> runCatchingCancellable(block: () -> T): Result<T> =
	try {
		Result.success(block())
	} catch (e: CancellationException) {
		throw e
	} catch (e: Exception) {
		Result.failure(e)
	}

/**
 * A message worth showing a user.
 *
 * [HttpException] matters here: Retrofit throws it for any non-2xx status
 * *before* the body is parsed, so a server that signals failure with an HTTP
 * status rather than a Subsonic error object never reaches [requireOk]. It is
 * also a RuntimeException, so it slips past an `IOException` catch - which is
 * exactly how it crashed the connection test.
 */
fun Throwable.userMessage(): String = when (this) {
	is SubsonicException -> message
	// The URL hint only fits a 404. Offering it for every status sent the
	// connection test's users to check an address that ping had just proved
	// correct, while the real cause - credentials the server choked on - went
	// unmentioned.
	is HttpException -> when (code()) {
		404 -> "Not found there. Check that the URL points at the server's root."
		else -> "The server answered HTTP ${code()}."
	}
	// A body that is not the JSON we asked for, which in practice means the
	// request never reached a gaindrive endpoint at all: an older server does
	// not have it, answers 404, and a reverse proxy configured to serve a single
	// page app turns that into its index.html with a 200 - HTML, and a
	// successful status, so nothing upstream of here objects.
	//
	// Worth its own branch rather than falling through: kotlinx's own wording is
	// "Expected start of the object '{' but had '<'", which sends the reader
	// looking for a parsing bug instead of a missing endpoint. Note it is a
	// RuntimeException, so it slips past the IOException catch below - the same
	// trap HttpException sets above.
	is SerializationException ->
		"The server did not answer in a form this app understands. It may be " +
			"too old for this feature."
	is UnknownHostException -> "Cannot find that host."
	is ConnectException -> "Nothing is listening at that address."
	is SocketTimeoutException -> "The server did not answer in time."
	// Above the IOException catch because it is one, and its default message -
	// a chain of certificate-path exceptions - explains nothing to anyone. A
	// self-hosted server behind a reverse proxy with its own certificate is the
	// common case; the other is a device on the LAN presenting one nothing
	// trusts, which is why `WiiMClient` brings its own trust manager.
	is SSLException -> "The secure connection was refused. The certificate may " +
		"be self-signed or issued for a different name."
	is IOException -> message ?: "Could not reach the server."
	else -> message ?: "Something went wrong."
}
