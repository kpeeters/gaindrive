package org.gaindrive.android.data

import org.gaindrive.android.net.SubsonicJson
import org.junit.Assert.assertArrayEquals
import org.junit.Assert.assertEquals
import org.junit.Assert.assertNull
import org.junit.Test

/**
 * The URIs the TV's pairing pane actually composes, plus the shapes anything
 * else could put in a VIEW intent - as with track links, the filter admits
 * the whole scheme and the parser is the boundary.
 */
class PairLinkTest {

	private val key = ByteArray(PAIR_KEY_BYTES) { it.toByte() }

	@Test
	fun `what buildPairUri writes parses back`() {
		val uri = buildPairUri("192.168.1.40", 48213, "0123456789abcdef", key)
		val link = parsePairLink(uri)!!
		assertEquals("192.168.1.40", link.host)
		assertEquals(48213, link.port)
		assertEquals("0123456789abcdef", link.token)
		assertArrayEquals(key, link.key)
	}

	@Test
	fun `a track link is not a pair link`() {
		assertNull(parsePairLink("gaindrive://my.server.net/?track=42"))
	}

	@Test
	fun `a pair link is not a track link`() {
		assertNull(parseTrackLink(buildPairUri("192.168.1.40", 48213, "tok", key)))
	}

	@Test
	fun `the wrong scheme is null`() {
		assertNull(parsePairLink("https://pair?host=1.2.3.4&port=80&t=x&k=AA"))
	}

	@Test
	fun `a missing parameter is null`() {
		val uri = buildPairUri("192.168.1.40", 48213, "tok", key)
		assertNull(parsePairLink(uri.replace("&t=tok", "")))
		assertNull(parsePairLink(uri.replace("host=192.168.1.40", "host=")))
	}

	@Test
	fun `a port outside the range is null`() {
		val uri = buildPairUri("192.168.1.40", 48213, "tok", key)
		assertNull(parsePairLink(uri.replace("port=48213", "port=0")))
		assertNull(parsePairLink(uri.replace("port=48213", "port=70000")))
	}

	@Test
	fun `a key of the wrong size is null`() {
		assertNull(parsePairLink("gaindrive://pair?host=1.2.3.4&port=80&t=tok&k=AAAA"))
	}

	@Test
	fun `a host smuggling a path or port is null`() {
		val uri = buildPairUri("192.168.1.40", 48213, "tok", key)
		assertNull(parsePairLink(uri.replace("host=192.168.1.40", "host=evil.example%2Fsteal")))
		assertNull(parsePairLink(uri.replace("host=192.168.1.40", "host=1.2.3.4%3A99")))
	}

	@Test
	fun `the payload round trips through json with fields it does not know`() {
		val payload = PairPayload(
			listOf(
				PairServer(
					name = "Home",
					url = "https://music.example.com",
					username = "kasper",
					password = "secret",
					browseByFolder = true,
				)
			)
		)
		val text = SubsonicJson.encodeToString(PairPayload.serializer(), payload)
		assertEquals(
			payload,
			SubsonicJson.decodeFromString(PairPayload.serializer(), text),
		)
		// A future field from a newer phone must not break an older TV. The
		// text ends `}]}`; the field is spliced into the server object.
		val extended = text.dropLast(3) + ""","newField":1}]}"""
		assertEquals(
			payload,
			SubsonicJson.decodeFromString(PairPayload.serializer(), extended),
		)
	}
}
