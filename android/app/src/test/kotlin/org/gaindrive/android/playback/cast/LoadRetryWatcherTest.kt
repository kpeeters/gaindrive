package org.gaindrive.android.playback.cast

import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Test

/**
 * The retry rules are the part of casting that took real debugging on the
 * server, and the failure they guard against is a track that
 * silently never starts. The decision is pure state, so it is checked here
 * rather than rediscovered against a receiver.
 *
 * Throughout: session 5 is the one being replaced, session 6 is the one our
 * LOAD created.
 */
class LoadRetryWatcherTest {

	private fun status(
		state: CastPlayerState,
		msid: Int,
		idleReason: String? = null,
	) = CastStatus(playerState = state, mediaSessionId = msid, idleReason = idleReason)

	@Test
	fun `an error on the new session triggers exactly one retry`() {
		val watcher = LoadRetryWatcher().apply { arm(5) }
		assertTrue(watcher.onStatus(status(CastPlayerState.IDLE, 6, "ERROR")))
		// A second error must not fire again; the retry is itself a fresh LOAD
		// that will arm the watcher anew if it wants one.
		assertFalse(watcher.onStatus(status(CastPlayerState.IDLE, 6, "ERROR")))
	}

	/**
	 * The whole reason the msid filter exists: a `GET_STATUS` poll fired just
	 * before the receiver processed our LOAD comes back as a healthy PLAYING for
	 * the *old* session. Acting on it would disarm the watcher, and the real
	 * error - which arrives afterwards, on the new session - would be ignored.
	 */
	@Test
	fun `a stale PLAYING for the old session does not disarm`() {
		val watcher = LoadRetryWatcher().apply { arm(5) }
		assertFalse(watcher.onStatus(status(CastPlayerState.PLAYING, 5)))
		assertTrue(watcher.isArmed)
		assertTrue(watcher.onStatus(status(CastPlayerState.IDLE, 6, "ERROR")))
	}

	/** IDLE/INTERRUPTED also carries the old id, and is skipped for free. */
	@Test
	fun `the interruption of the old session is not an error`() {
		val watcher = LoadRetryWatcher().apply { arm(5) }
		assertFalse(watcher.onStatus(status(CastPlayerState.IDLE, 5, "INTERRUPTED")))
		assertTrue(watcher.isArmed)
	}

	@Test
	fun `a healthy load disarms the watcher`() {
		for (state in listOf(
			CastPlayerState.PLAYING,
			CastPlayerState.BUFFERING,
			CastPlayerState.LOADING,
		)) {
			val watcher = LoadRetryWatcher().apply { arm(5) }
			assertFalse(watcher.onStatus(status(state, 6)))
			assertFalse("$state should disarm", watcher.isArmed)
		}
	}

	/**
	 * A paused new session is neither the failure we retry nor proof the load
	 * took, so it decides nothing and the watcher stays armed.
	 */
	@Test
	fun `pause decides nothing`() {
		val watcher = LoadRetryWatcher().apply { arm(5) }
		assertFalse(watcher.onStatus(status(CastPlayerState.PAUSED, 6)))
		assertTrue(watcher.isArmed)
	}

	@Test
	fun `a finished track is not a failed load`() {
		val watcher = LoadRetryWatcher().apply { arm(5) }
		assertFalse(watcher.onStatus(status(CastPlayerState.IDLE, 6, "FINISHED")))
	}

	@Test
	fun `nothing fires while disarmed`() {
		val watcher = LoadRetryWatcher()
		assertFalse(watcher.onStatus(status(CastPlayerState.IDLE, 6, "ERROR")))

		watcher.arm(5)
		watcher.disarm()
		assertFalse(watcher.onStatus(status(CastPlayerState.IDLE, 6, "ERROR")))
	}
}
