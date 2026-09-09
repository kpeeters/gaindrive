package org.gaindrive.android.data

import org.gaindrive.android.data.model.ServerId
import org.gaindrive.android.net.FetchJobDto
import org.junit.Assert.assertEquals
import org.junit.Assert.assertTrue
import org.junit.Test

/**
 * The rule that decides whether the uploads listing is re-read.
 *
 * Both directions are silent failures rather than crashes, which is why they
 * are worth pinning: miss a transition and a finished fetch never appears until
 * something else happens to reload, and fire on every sighting instead and the
 * listing reloads every two seconds for the quarter hour the server retains a
 * finished job.
 */
class FetchMonitorTest {

	private val server = ServerId("s1")

	private fun ref(id: String, state: String) =
		FetchJobRef(server, "Home", FetchJobDto(id = id, state = state))

	@Test
	fun `a first sighting is not a completion`() {
		val seen = mutableMapOf<String, String>()
		// Already finished the very first time we looked: it landed before
		// anyone was watching, and the browse load after launch covers it.
		val done = completedSince(seen, listOf(ref("a", "done")))
		assertTrue(done.isEmpty())
		assertEquals("done", seen["a"])
	}

	@Test
	fun `a transition into done is reported once`() {
		val seen = mutableMapOf<String, String>()
		completedSince(seen, listOf(ref("a", "running")))

		val first = completedSince(seen, listOf(ref("a", "done")))
		assertEquals(listOf("a"), first.map { it.job.id })

		// The server goes on reporting it for fifteen minutes.
		val second = completedSince(seen, listOf(ref("a", "done")))
		assertTrue(second.isEmpty())
	}

	@Test
	fun `a job that finishes while nothing is polling still counts`() {
		// The app was backgrounded at 'running' and foregrounded at 'done', with
		// no tick in between. The map outlives the loop precisely so this works.
		val seen = mutableMapOf<String, String>()
		completedSince(seen, listOf(ref("a", "queued")))
		completedSince(seen, listOf(ref("a", "running")))

		val done = completedSince(seen, listOf(ref("a", "done")))
		assertEquals(listOf("a"), done.map { it.job.id })
	}

	@Test
	fun `failing and cancelling are not completions`() {
		val seen = mutableMapOf<String, String>()
		completedSince(seen, listOf(ref("a", "running"), ref("b", "running")))

		// Neither moved anything onto disk, so neither is a reason to re-read
		// the library.
		val done = completedSince(seen, listOf(ref("a", "error"), ref("b", "cancelled")))
		assertTrue(done.isEmpty())
	}

	@Test
	fun `several jobs are tracked apart`() {
		val seen = mutableMapOf<String, String>()
		completedSince(seen, listOf(ref("a", "running"), ref("b", "queued")))

		val done = completedSince(seen, listOf(ref("a", "done"), ref("b", "running")))
		assertEquals(listOf("a"), done.map { it.job.id })
	}
}
