package org.gaindrive.android.data

import kotlinx.coroutines.sync.Mutex
import kotlinx.coroutines.sync.withLock
import org.gaindrive.android.data.model.MusicRoot
import org.gaindrive.android.data.model.ServerId
import javax.inject.Inject
import javax.inject.Singleton

/**
 * Each server's configured roots, fetched once and kept for the session.
 *
 * Roots are configuration: they change when someone edits the server, not while
 * someone is browsing it. Re-asking on every chip press would put a request in
 * front of a control that should feel instant.
 *
 * It lives here rather than inside `LibraryRepository` — where it began — for
 * one structural reason: the repository injects `ServerRegistry`, so the
 * registry cannot reach back into it to invalidate a server whose browse mode
 * has just changed. A singleton of its own is injectable by both, and it is the
 * same shape as the other two per-server caches the registry already clears,
 * `SubsonicClientFactory.forget` and `LocalLibrary.forgetServer`.
 */
@Singleton
class MusicRoots @Inject constructor() {

	private val cache = mutableMapOf<ServerId, List<MusicRoot>>()
	private val mutex = Mutex()

	/**
	 * The roots of [server], fetching them with [load] the first time.
	 *
	 * A failure caches nothing, so a server that was briefly unreachable is
	 * asked again rather than being remembered as having no roots at all — which
	 * would quietly hold its chips back for the rest of the session.
	 */
	suspend fun of(server: ServerId, load: suspend () -> List<MusicRoot>): List<MusicRoot> {
		cache[server]?.let { return it }
		return mutex.withLock {
			cache[server] ?: load().also { cache[server] = it }
		}
	}

	fun forget(server: ServerId) {
		cache.remove(server)
	}
}
