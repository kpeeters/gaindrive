package org.gaindrive.android.data

import kotlinx.coroutines.CancellationException
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.withContext
import kotlinx.coroutines.withTimeoutOrNull
import org.gaindrive.android.net.ConnectionTest
import org.gaindrive.android.net.SubsonicClient
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

	suspend fun test(
		url: String,
		username: String,
		password: String,
		/** Tests the endpoints this server will actually be browsed by. */
		browseByFolder: Boolean,
	): ConnectionTest =
		withContext(Dispatchers.IO) {
			try {
				val client = clients.transientClient(url, username, password)
				client.ping().requireOk()
				verify(client, browseByFolder)
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

	/**
	 * Asks for the artist list, because a successful `ping` is not evidence
	 * that the credentials are any good.
	 *
	 * Bandcamp's implementation answers `ping` with `ok` for any username and
	 * password at all, and then fails every endpoint that actually looks the
	 * user up. A test that only pinged went green on credentials the app could
	 * not use — which is the one thing this button exists to catch.
	 *
	 * The artist list specifically: it is the first thing the app really does, so
	 * a server that passes here cannot fail on the first screen. Which endpoint
	 * that is follows the browse setting, or the button would go green on the
	 * half of the API this server is not going to be asked.
	 */
	private suspend fun verify(
		client: SubsonicClient,
		browseByFolder: Boolean,
	): ConnectionTest {
		val outcome = withTimeoutOrNull(VERIFY_TIMEOUT_MS) {
			try {
				if (browseByFolder) client.getIndexes(null, null, null).requireOk()
				else client.getArtists(null, null, null).requireOk()
				ConnectionTest.Reachable
			} catch (e: CancellationException) {
				throw e
			} catch (e: Exception) {
				// Reported as a rejection, not as unreachable: ping has just
				// proved there is a server at that address, so whatever went
				// wrong here is about this request, not the address.
				ConnectionTest.Rejected(e.userMessage())
			}
		}
		// Slowness is not refusal. Bandcamp warns that a large collection is
		// slow to list in their beta, and holding the dialog open for the full
		// read would make the button useless on exactly those accounts.
		return outcome ?: ConnectionTest.Unverified
	}

	private companion object {
		/**
		 * Long enough for a slow library to answer, short enough that the user
		 * is not left watching a spinner wondering whether it hung.
		 */
		const val VERIFY_TIMEOUT_MS = 8_000L
	}
}
