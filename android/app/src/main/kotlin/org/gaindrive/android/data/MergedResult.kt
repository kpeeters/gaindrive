package org.gaindrive.android.data

import org.gaindrive.android.data.model.ServerConfig
import org.gaindrive.android.data.model.ServerId

/**
 * A server that did not answer, and why, in words the UI can show.
 *
 * [message] also says whether a stored copy was put on screen in its place.
 * Carrying that in the message rather than as a flag on [MergedResult] means
 * every screen that already shows these gets it without any of them having to
 * learn what a mirror is.
 */
data class ServerFailure(
	val server: ServerId,
	val serverName: String,
	val message: String,
)

/**
 * The result of a query that may have spanned several servers.
 *
 * Partial failure is representable from the start rather than bolted on: with
 * several servers, one being unreachable has to degrade the view instead of
 * emptying it. A screen must never be blank because the least important of
 * three servers is down.
 */
data class MergedResult<T>(
	val items: T,
	val failures: List<ServerFailure> = emptyList(),
) {
	val isPartial: Boolean get() = failures.isNotEmpty()

	fun <R> map(transform: (T) -> R): MergedResult<R> =
		MergedResult(transform(items), failures)
}

/**
 * One server's contribution, kept separate rather than interleaved.
 *
 * Playlists, starred items and recents are per-account server-side state. A
 * global ordering across servers would look right and be wrong: each server
 * only knows what happened against it, so interleaving by timestamp implies a
 * completeness that does not exist.
 */
data class ServerSection<T>(
	val server: ServerConfig,
	val items: List<T>,
)
