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
 * Roles are per server - the same person can be an admin on one and a
 * restricted account on another - so nothing here is a global fact about the
 * user.
 */
data class AccountFacts(
	/** May write into the personal uploads area. Admins may regardless. */
	val canUpload: Boolean = false,
	val isAdmin: Boolean = false,
) {
	companion object {
		/**
		 * What an unreachable server is assumed to be: permitted nothing. A
		 * permission guessed *present* would offer a control that fails when
		 * used.
		 */
		val UNKNOWN = AccountFacts()
	}
}

/**
 * One `getUser` per server per launch, answering everything the app needs to
 * know about its own account there.
 *
 * [canUpload] decides whether the Uploads slice is offered at all, and
 * [isAdmin] whether an upload can be moved into the shared library.
 *
 * Held in memory for the process rather than persisted: it is one cheap call
 * per server per launch, it picks up a change made on the server without any
 * invalidation logic, and there is nothing to migrate. The timeout is short
 * because a slice that fails to appear is recovered by relaunching rather than
 * by waiting.
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
		// Re-asking costs one request on the next lookup, and it is what
		// makes a slice hidden by a momentary timeout come back by itself
		// rather than needing the app relaunched.
		fetch(config)?.also { known[config.id] = it } ?: AccountFacts.UNKNOWN
	}

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
					// An admin may upload whether or not the role is set, which
					// is how the server itself reads it - see check_upload_perm.
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
