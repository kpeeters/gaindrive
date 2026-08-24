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
 * What the signed-in account may do on one server.
 *
 * Roles are per server — the same person can be an admin on one and a
 * restricted account on another — so nothing here is a global fact about the
 * user.
 */
data class AccountFacts(
	/** 0 means no limit, and is also what every failure resolves to. */
	val maxBitRate: Int = 0,
	/** May write into the personal uploads area. Admins may regardless. */
	val canUpload: Boolean = false,
	val isAdmin: Boolean = false,
) {
	companion object {
		/**
		 * What an unreachable server is assumed to be: uncapped, and permitted
		 * nothing.
		 *
		 * The two halves are guessed in opposite directions on purpose.
		 * Uncapped is what the overwhelming majority of accounts are, and
		 * guessing a cap that is not there would degrade every stream for the
		 * rest of the session. A permission guessed *present* would instead
		 * offer a control that fails when used, so those default to absent.
		 */
		val UNKNOWN = AccountFacts()
	}
}

/**
 * One `getUser` per server per launch, answering everything the app needs to
 * know about its own account there.
 *
 * The bit rate is the reason this exists: the server enforces its ceiling
 * whatever the client asks for, so the app has to know it before it can name
 * the quality it is about to receive — a request for the original file on a
 * capped account comes back as MP3 at the cap, and bytes stored under the wrong
 * quality key are worse than bytes not stored at all. The roles came later and
 * ride along rather than costing a second request: [canUpload] decides whether
 * the Uploads slice is offered at all, and [isAdmin] whether an upload can be
 * moved into the shared library.
 *
 * Held in memory for the process rather than persisted: it is one cheap call
 * per server per launch, it picks up a change made on the server without any
 * invalidation logic, and there is nothing to migrate. The first stream URL of
 * a session waits for it, which is why the timeout is short — offline playback
 * comes from the cache anyway, and a slice that fails to appear is recovered by
 * relaunching rather than by waiting.
 */
@Singleton
class Accounts @Inject constructor(
	private val clients: SubsonicClientFactory,
) {

	private val mutex = Mutex()
	private val known = mutableMapOf<ServerId, AccountFacts>()

	suspend fun factsFor(config: ServerConfig): AccountFacts = mutex.withLock {
		known[config.id]?.let { return@withLock it }
		// A server that did not answer is not remembered as having answered.
		// Re-asking costs one request on the next stream URL, and it is what
		// makes a slice hidden by a momentary timeout come back by itself
		// rather than needing the app relaunched.
		fetch(config)?.also { known[config.id] = it } ?: AccountFacts.UNKNOWN
	}

	/** 0 means no limit. */
	suspend fun capFor(config: ServerConfig): Int = factsFor(config).maxBitRate

	/** Whether this account may put anything in its own uploads area here. */
	suspend fun canUploadTo(config: ServerConfig): Boolean = factsFor(config).canUpload

	suspend fun isAdminOn(config: ServerConfig): Boolean = factsFor(config).isAdmin

	/** Drops a cached answer, so an account whose roles just changed is re-asked. */
	suspend fun forget(server: ServerId) = mutex.withLock {
		known.remove(server)
		Unit
	}

	/** Null when the server did not answer, which is not a fact about it. */
	private suspend fun fetch(config: ServerConfig): AccountFacts? =
		withTimeoutOrNull(FETCH_TIMEOUT_MS) {
			try {
				val user = clients.clientFor(config)
					.getUser(config.username)
					.requireOk()
					.user ?: return@withTimeoutOrNull null
				AccountFacts(
					maxBitRate = user.maxBitRate.takeIf { it > 0 } ?: 0,
					// An admin may upload whether or not the role is set, which
					// is how the server itself reads it — see check_upload_perm.
					canUpload = user.uploadRole || user.adminRole,
					isAdmin = user.adminRole,
				)
			} catch (e: CancellationException) {
				throw e
			} catch (e: Exception) {
				null
			}
		}

	private companion object {
		const val FETCH_TIMEOUT_MS = 2_000L
	}
}
