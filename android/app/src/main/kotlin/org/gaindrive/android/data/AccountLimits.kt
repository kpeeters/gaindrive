package org.gaindrive.android.data

import kotlinx.coroutines.CancellationException
import kotlinx.coroutines.sync.Mutex
import kotlinx.coroutines.sync.withLock
import kotlinx.coroutines.withTimeoutOrNull
import org.gaindrive.android.data.model.ServerConfig
import org.gaindrive.android.data.model.ServerId
import org.gaindrive.android.net.SubsonicClientFactory
import org.gaindrive.android.net.requireOk
import javax.inject.Inject
import javax.inject.Singleton

/**
 * Each server's `maxBitRate` ceiling for the signed-in account.
 *
 * The server enforces this whatever the client asks for, so the app has to know
 * it before it can name the quality it is about to receive — a request for the
 * original file on a capped account comes back as MP3 at the cap, and bytes
 * stored under the wrong quality key are worse than bytes not stored at all.
 *
 * Held in memory for the process rather than persisted: it is one cheap call
 * per server per launch, it picks up a change made on the server without any
 * invalidation logic, and there is nothing to migrate. The first stream URL of
 * a session waits for it, which is why the timeout is short — an unreachable
 * server means uncapped, and offline playback comes from the cache anyway.
 */
@Singleton
class AccountLimits @Inject constructor(
	private val clients: SubsonicClientFactory,
) {

	private val mutex = Mutex()
	private val caps = mutableMapOf<ServerId, Int>()

	/** 0 means no limit, and is also what every failure resolves to. */
	suspend fun capFor(config: ServerConfig): Int = mutex.withLock {
		caps.getOrPut(config.id) { fetch(config) }
	}

	private suspend fun fetch(config: ServerConfig): Int {
		val cap = withTimeoutOrNull(FETCH_TIMEOUT_MS) {
			try {
				clients.clientFor(config)
					.getUser(config.username)
					.requireOk()
					.user
					?.maxBitRate
			} catch (e: CancellationException) {
				throw e
			} catch (e: Exception) {
				// Uncapped is the right guess when we cannot ask: it is what
				// the overwhelming majority of accounts are, and guessing a cap
				// that is not there would degrade every stream on this server
				// for the rest of the session.
				null
			}
		}
		return cap?.takeIf { it > 0 } ?: 0
	}

	private companion object {
		const val FETCH_TIMEOUT_MS = 2_000L
	}
}
