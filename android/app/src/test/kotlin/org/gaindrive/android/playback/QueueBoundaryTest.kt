package org.gaindrive.android.playback

import org.junit.Assert.assertEquals
import org.junit.Assert.assertNull
import org.junit.Test

/**
 * The queue boundary decides what "add to queue" is allowed to throw away, so
 * an off-by-one here loses tracks the user chose. Cheap to test, and it is the
 * one part of the player that is pure arithmetic.
 */
class QueueBoundaryTest {

	@Test
	fun `playing an album marks the remainder automatic`() {
		// Album of 10, tapped at track 3 (index 2): 0..2 are wanted, 3.. is tail.
		assertEquals(3, QueueBoundary.EMPTY.afterPlay(2).value)
	}

	@Test
	fun `playing the first track leaves only it manual`() {
		assertEquals(1, QueueBoundary.EMPTY.afterPlay(0).value)
	}

	@Test
	fun `the automatic tail is what gets dropped on append`() {
		// Queue of 10 from tapping track 3: drop indices 3 through 9.
		val boundary = QueueBoundary(3)
		assertEquals(3..9, boundary.tailToDrop(10))
	}

	/** Nothing automatic means nothing to discard. */
	@Test
	fun `no tail when the whole queue is hand-picked`() {
		assertNull(QueueBoundary(4).tailToDrop(4))
		assertNull(QueueBoundary.adoptingExisting(7).tailToDrop(7))
	}

	@Test
	fun `appending extends the manual region to the whole queue`() {
		assertEquals(5, QueueBoundary(3).afterAppend(newCount = 5).value)
	}

	/**
	 * The sequence that matters: play an album, then queue two tracks. The
	 * album tail is dropped once and both hand-picked tracks survive.
	 */
	@Test
	fun `queueing twice keeps both tracks`() {
		var boundary = QueueBoundary.EMPTY.afterPlay(startIndex = 0)
		assertEquals(1, boundary.value)

		// Queue of 10; drop 1..9, leaving 1, then append -> 2.
		assertEquals(1..9, boundary.tailToDrop(10))
		boundary = boundary.afterAppend(newCount = 2)
		assertEquals(2, boundary.value)

		// Second append: nothing automatic left to drop.
		assertNull(boundary.tailToDrop(2))
		boundary = boundary.afterAppend(newCount = 3)
		assertEquals(3, boundary.value)
	}

	@Test
	fun `inserting into the automatic region makes that slot manual`() {
		// Boundary at 1, inserting after the current track at index 1.
		assertEquals(2, QueueBoundary(1).afterInsert(at = 1).value)
	}

	@Test
	fun `inserting inside the manual region just lengthens it`() {
		assertEquals(6, QueueBoundary(5).afterInsert(at = 2).value)
	}

	@Test
	fun `removing before the boundary shifts it down`() {
		assertEquals(2, QueueBoundary(3).afterRemove(index = 0).value)
	}

	@Test
	fun `removing after the boundary leaves it alone`() {
		assertEquals(3, QueueBoundary(3).afterRemove(index = 7).value)
		// The boundary is the first automatic entry, so removing it is a
		// removal from the tail, not from the manual region.
		assertEquals(3, QueueBoundary(3).afterRemove(index = 3).value)
	}

	/**
	 * A queue restored after process death is treated as entirely hand-picked,
	 * so the next enqueue cannot wipe it.
	 */
	@Test
	fun `an adopted queue has no automatic tail`() {
		val boundary = QueueBoundary.adoptingExisting(12)
		assertEquals(12, boundary.value)
		assertNull(boundary.tailToDrop(12))
	}
}
