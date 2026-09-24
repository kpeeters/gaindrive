package org.gaindrive.android.playback.cast

/**
 * Decides whether a LOAD that appears to have failed should be sent again.
 *
 * The Default Media Receiver intermittently fails the first LOAD that lands
 * while another media session is `PLAYING`: it creates a new `mediaSessionId`
 * for our LOAD, and that session goes from `IDLE`/`INTERRUPTED` straight to
 * `IDLE`/`ERROR` without ever reaching `PLAYING`. Re-sending the same LOAD into
 * the now-quiet receiver works, and matches what a user does by hand when they
 * tap the same track twice. The behaviour took long debugging on the server;
 * this is the Kotlin half of the port from `CastManager::update_status`.
 *
 * **The msid filter is the load-bearing part.** Statuses arriving for the
 * *superseded* session must not be consulted at all. A `GET_STATUS` poll fired
 * just before the receiver processed our LOAD comes back as a healthy `PLAYING`
 * carrying the old session id; treating it as evidence would disarm the watcher,
 * and the real `IDLE`/`ERROR` for the new session - which arrives later - would
 * then be ignored. The intermediate `IDLE`/`INTERRUPTED` push carries the old id
 * too and is skipped for the same reason, which costs nothing: the next push,
 * the new session reaching either `PLAYING` or `ERROR`, is the one that decides.
 *
 * Pure state, no I/O, so the decision table is unit tested rather than
 * rediscovered against a real receiver.
 */
internal class LoadRetryWatcher {

	private var pending = false
	private var supersededMsid = 0

	val isArmed: Boolean get() = pending

	/**
	 * Arms the watcher for a LOAD that is about to be sent. [currentMsid] is the
	 * session the LOAD will replace - status pushes still carrying it are stale
	 * by definition.
	 */
	fun arm(currentMsid: Int) {
		pending = true
		supersededMsid = currentMsid
	}

	fun disarm() {
		pending = false
	}

	/**
	 * Feeds a status in. Returns true exactly once, when the LOAD should be
	 * re-sent; any conclusive outcome disarms the watcher either way.
	 *
	 * `PAUSED` deliberately decides nothing: it is neither the failure we retry
	 * nor proof the load took, and leaving the watcher armed costs nothing.
	 */
	fun onStatus(status: CastStatus): Boolean {
		if (!pending) return false
		if (status.mediaSessionId == supersededMsid) return false

		if (status.isIdleError) {
			pending = false
			return true
		}
		if (status.isLive) {
			pending = false
		}
		return false
	}
}
