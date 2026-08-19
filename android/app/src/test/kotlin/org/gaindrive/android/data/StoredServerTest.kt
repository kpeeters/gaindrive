package org.gaindrive.android.data

import kotlinx.serialization.decodeFromString
import kotlinx.serialization.encodeToString
import org.gaindrive.android.net.SubsonicJson
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Test

/**
 * The persisted shape of the server list.
 *
 * Worth pinning because the failure mode is silent and total: `ServerStore`
 * falls back to an empty list when the document does not decode, so a field
 * added without a default would not raise anything — it would quietly discard
 * every server the user had configured.
 */
class StoredServerTest {

	private val json = SubsonicJson

	@Test
	fun `a document written before folder browsing decodes with it off`() {
		val legacy = """[{"id":"a","name":"Home","url":"https://h","username":"kasper",
		                  "password":"cipher","enabled":true}]"""
		val servers = json.decodeFromString<List<StoredServer>>(legacy)
		assertEquals(1, servers.size)
		assertFalse(servers[0].browseByFolder)
		assertEquals("Home", servers[0].name)
	}

	/** The oldest documents predate the password and enabled fields too. */
	@Test
	fun `a document with only the first four fields still decodes`() {
		val ancient = """[{"id":"a","name":"Home","url":"https://h","username":"kasper"}]"""
		val servers = json.decodeFromString<List<StoredServer>>(ancient)
		assertTrue(servers[0].enabled)
		assertFalse(servers[0].browseByFolder)
	}

	@Test
	fun `the flag survives a round trip`() {
		val original = listOf(
			StoredServer("a", "Home", "https://h", "kasper", browseByFolder = true)
		)
		val restored = json.decodeFromString<List<StoredServer>>(json.encodeToString(original))
		assertEquals(original, restored)
	}
}
