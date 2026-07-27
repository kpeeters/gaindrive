package org.gaindrive.android.data.model

import org.junit.Assert.assertEquals
import org.junit.Assert.assertNotEquals
import org.junit.Assert.assertNull
import org.junit.Test

/**
 * A bug here is invisible with one server configured and shows up as tracks
 * from the wrong library the day a second one is added, so it is worth testing
 * long before there is a second server.
 */
class IdsTest {

	@Test
	fun `ref survives a media id round trip`() {
		val ref = ItemRef(ServerId("3f2b7c10-0000-4000-8000-000000000001"), "42")
		assertEquals(ref, ItemRef.decode(ref.encode()))
	}

	@Test
	fun `same id on different servers is not the same ref`() {
		val a = ItemRef(ServerId("server-a"), "42")
		val b = ItemRef(ServerId("server-b"), "42")
		assertNotEquals(a, b)
		assertNotEquals(a.encode(), b.encode())
	}

	@Test
	fun `malformed media ids decode to null rather than throwing`() {
		assertNull(ItemRef.decode(""))
		assertNull(ItemRef.decode("42"))
		assertNull(ItemRef.decode("/42"))
		assertNull(ItemRef.decode("server-a/"))
	}

	/** Splitting on the first separator keeps ids containing one intact. */
	@Test
	fun `only the first separator splits`() {
		val decoded = ItemRef.decode("server-a/nested/id")
		assertEquals(ServerId("server-a"), decoded?.server)
		assertEquals("nested/id", decoded?.id)
	}

	@Test
	fun `generated server ids are distinct`() {
		assertNotEquals(ServerId.new(), ServerId.new())
	}
}
