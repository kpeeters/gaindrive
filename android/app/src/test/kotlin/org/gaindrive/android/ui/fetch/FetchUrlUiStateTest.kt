package org.gaindrive.android.ui.fetch

import org.gaindrive.android.data.FetchJobRef
import org.gaindrive.android.data.FetchStatus
import org.gaindrive.android.data.model.ServerId
import org.gaindrive.android.net.FetchJobDto
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Test

/**
 * What the panel refuses, now that it adopts jobs it did not start.
 *
 * The refusal is deliberately narrow - this URL, already being fetched - and
 * the two failures either side of that line are both real. Too wide and a fetch
 * begun in the web client freezes the form on the phone; too narrow and the
 * duplicate this whole change exists to prevent comes back.
 */
class FetchUrlUiStateTest {

	private val server = ServerId("s1")
	private val other = ServerId("s2")

	private fun job(id: String, url: String, state: String) =
		FetchJobDto(id = id, url = url, state = state)

	private fun status(vararg jobs: Pair<ServerId, FetchJobDto>) =
		FetchStatus(jobs.map { (s, j) -> FetchJobRef(s, "Home", j) })

	private fun state(url: String, status: FetchStatus) =
		FetchUrlUiState(url = url, server = server, status = status)

	@Test
	fun `a live job for this URL blocks submitting`() {
		val s = state("https://x/1", status(server to job("a", "https://x/1", "running")))
		assertEquals("a", s.duplicate?.id)
		assertFalse(s.canSubmit)
	}

	@Test
	fun `a live job for another URL does not`() {
		// The failure this guards against: the form used to lock behind *any*
		// running job, which once it adopts other sessions' work means somebody
		// else's fetch disables the phone in front of you.
		val s = state("https://x/2", status(server to job("a", "https://x/1", "running")))
		assertNull(s.duplicate)
		assertTrue(s.canSubmit)
	}

	@Test
	fun `a finished job for this URL is reported but never blocks`() {
		val s = state("https://x/1", status(server to job("a", "https://x/1", "done")))
		assertEquals("a", s.duplicate?.id)
		assertTrue(s.canSubmit)
	}

	@Test
	fun `a live job outranks a finished one for the same URL`() {
		val s = state(
			"https://x/1",
			status(
				server to job("old", "https://x/1", "done"),
				server to job("new", "https://x/1", "queued"),
			),
		)
		assertEquals("new", s.duplicate?.id)
		assertFalse(s.canSubmit)
	}

	@Test
	fun `another server's job is not this server's duplicate`() {
		val s = state("https://x/1", status(other to job("a", "https://x/1", "running")))
		assertNull(s.duplicate)
		assertTrue(s.canSubmit)
	}

	@Test
	fun `the URL is compared trimmed and otherwise exactly`() {
		val st = status(server to job("a", "https://x/1", "running"))
		// Trimmed, because a pasted URL routinely arrives with whitespace.
		assertEquals("a", state("  https://x/1  ", st).duplicate?.id)
		// And not normalised: the server compares the strings as they arrive, so
		// refusing more than it would makes this client lie about it.
		assertNull(state("https://x/1?utm=1", st).duplicate)
	}

	@Test
	fun `an empty URL matches nothing`() {
		val s = state("   ", status(server to job("a", "https://x/1", "running")))
		assertNull(s.duplicate)
		assertFalse(s.canSubmit)
	}
}
