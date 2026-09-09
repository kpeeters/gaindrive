package org.gaindrive.android.data.model

import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Test

/**
 * `FetchMonitor`'s poll loop, the panel's Cancel button and the shell's fetch
 * strip all branch on this, so a mis-mapped state is either a fetch the user
 * cannot stop, a poll that never ends, or a strip that never clears.
 */
class FetchStateTest {

	@Test
	fun `every state the server can send is recognised`() {
		assertEquals(FetchState.QUEUED, FetchState.of("queued"))
		assertEquals(FetchState.RUNNING, FetchState.of("running"))
		assertEquals(FetchState.SCANNING, FetchState.of("scanning"))
		assertEquals(FetchState.DONE, FetchState.of("done"))
		assertEquals(FetchState.ERROR, FetchState.of("error"))
		assertEquals(FetchState.CANCELLED, FetchState.of("cancelled"))
	}

	@Test
	fun `anything else is unknown rather than an exception`() {
		assertEquals(FetchState.UNKNOWN, FetchState.of("paused"))
		assertEquals(FetchState.UNKNOWN, FetchState.of(""))
	}

	/**
	 * Scanning counts as live: the server does it before reporting `done`
	 * precisely so that `done` means the library is already correct, and a poll
	 * that stopped at the end of the download would report success too early.
	 */
	@Test
	fun `live covers the three states that are still working`() {
		assertTrue(FetchState.QUEUED.isLive)
		assertTrue(FetchState.RUNNING.isLive)
		assertTrue(FetchState.SCANNING.isLive)
		assertFalse(FetchState.DONE.isLive)
		assertFalse(FetchState.ERROR.isLive)
		assertFalse(FetchState.CANCELLED.isLive)
		// A state we do not understand must not keep the poll running for ever.
		assertFalse(FetchState.UNKNOWN.isLive)
	}

	/**
	 * The server refuses to cancel a scan — the download is already done and the
	 * files are being indexed — so offering the button there would be a control
	 * that fails.
	 */
	@Test
	fun `only queued and running may be cancelled`() {
		assertTrue(FetchState.QUEUED.isCancellable)
		assertTrue(FetchState.RUNNING.isCancellable)
		assertFalse(FetchState.SCANNING.isCancellable)
		assertFalse(FetchState.DONE.isCancellable)
	}
}
