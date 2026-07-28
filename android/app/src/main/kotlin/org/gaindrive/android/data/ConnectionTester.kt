package org.gaindrive.android.data

import kotlinx.coroutines.CancellationException
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.withContext
import org.gaindrive.android.net.ConnectionTest
import org.gaindrive.android.net.SubsonicClientFactory
import org.gaindrive.android.net.SubsonicException
import org.gaindrive.android.net.requireOk
import org.gaindrive.android.net.userMessage
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
			} catch (e: CancellationException) {
				throw e
			} catch (e: SubsonicException) {
				// The server answered and refused: bad password, disabled
				// account, and so on.
				ConnectionTest.Rejected(e.message)
			} catch (e: Exception) {
				// Everything else is "we could not have a conversation with a
				// GainDrive server at that address". Catching Exception rather
				// than IOException is deliberate: Retrofit's HttpException is a
				// RuntimeException, and a bad URL throws IllegalArgumentException
				// out of Retrofit's builder. Neither may reach the user as a
				// crash — this dialog exists precisely to report them.
				ConnectionTest.Unreachable(e.userMessage())
			}
		}
}
