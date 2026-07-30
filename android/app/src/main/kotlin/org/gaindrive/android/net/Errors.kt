package org.gaindrive.android.net

import retrofit2.HttpException
import java.io.IOException
import java.net.ConnectException
import java.net.SocketTimeoutException
import java.net.UnknownHostException
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
 * also a RuntimeException, so it slips past an `IOException` catch — which is
 * exactly how it crashed the connection test.
 */
fun Throwable.userMessage(): String = when (this) {
	is SubsonicException -> message
	// The URL hint only fits a 404. Offering it for every status sent the
	// connection test's users to check an address that ping had just proved
	// correct, while the real cause — credentials the server choked on — went
	// unmentioned.
	is HttpException -> when (code()) {
		404 -> "Not found there. Check that the URL points at the server's root."
		else -> "The server answered HTTP ${code()}."
	}
	is UnknownHostException -> "Cannot find that host."
	is ConnectException -> "Nothing is listening at that address."
	is SocketTimeoutException -> "The server did not answer in time."
	is IOException -> message ?: "Could not reach the server."
	else -> message ?: "Something went wrong."
}
