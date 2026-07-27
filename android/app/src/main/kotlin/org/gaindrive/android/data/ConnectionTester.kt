package org.gaindrive.android.data

import kotlinx.coroutines.CancellationException
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.withContext
import org.gaindrive.android.net.ConnectionTest
import org.gaindrive.android.net.SubsonicClientFactory
import org.gaindrive.android.net.SubsonicException
import org.gaindrive.android.net.requireOk
import java.io.IOException
import javax.inject.Inject
import javax.inject.Singleton

/**
 * Checks whether a set of credentials actually works, without saving them.
 *
 * The distinction the UI needs is between "the server said no" and "there was
 * no server" — those call for different fixes, and a single "failed" message
 * makes the user guess.
 */
@Singleton
class ConnectionTester @Inject constructor(
	private val clients: SubsonicClientFactory,
) {

	suspend fun test(url: String, username: String, password: String): ConnectionTest =
		withContext(Dispatchers.IO) {
			try {
				clients.transientClient(url, username, password).ping().requireOk()
				ConnectionTest.Reachable
			} catch (e: SubsonicException) {
				ConnectionTest.Rejected(e.message)
			} catch (e: CancellationException) {
				throw e
			} catch (e: IOException) {
				ConnectionTest.Unreachable(e.message ?: "Could not reach the server")
			} catch (e: IllegalStateException) {
				// Thrown by SubsonicClient when the URL is not parseable at all.
				ConnectionTest.Unreachable(e.message ?: "That does not look like a URL")
			} catch (e: IllegalArgumentException) {
				// Retrofit rejects a base URL it cannot parse.
				ConnectionTest.Unreachable("That does not look like a URL")
			}
		}
}
