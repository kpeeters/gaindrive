package org.gaindrive.android.data

import org.gaindrive.android.data.model.ItemRef
import org.gaindrive.android.data.model.ServerId
import org.junit.Assert.assertEquals
import org.junit.Test

/**
 * The reconcile rule is what turns a star the server silently ignored back
 * into an outline, so it is the part of the store worth pinning down.
 */
class StarStoreTest {

	private val a = ServerId("server-a")
	private val b = ServerId("server-b")

	@Test
	fun `an override the server agrees with is kept`() {
		val ref = ItemRef(a, "1")
		val result = StarStore.reconciled(mapOf(ref to true), a, setOf(ref))
		assertEquals(mapOf(ref to true), result)
	}

	@Test
	fun `a star the server did not record reverts`() {
		val ref = ItemRef(a, "1")
		val result = StarStore.reconciled(mapOf(ref to true), a, emptySet())
		assertEquals(mapOf(ref to false), result)
	}

	@Test
	fun `an unstar the server did not record reverts`() {
		val ref = ItemRef(a, "1")
		val result = StarStore.reconciled(mapOf(ref to false), a, setOf(ref))
		assertEquals(mapOf(ref to true), result)
	}

	/** Same id on another server is a different item and another account's answer. */
	@Test
	fun `other servers are left alone`() {
		val onA = ItemRef(a, "1")
		val onB = ItemRef(b, "1")
		val result = StarStore.reconciled(mapOf(onA to true, onB to true), a, emptySet())
		assertEquals(mapOf(onA to false, onB to true), result)
	}
}
