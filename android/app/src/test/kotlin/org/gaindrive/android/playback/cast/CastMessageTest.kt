package org.gaindrive.android.playback.cast

import org.junit.Assert.assertArrayEquals
import org.junit.Assert.assertEquals
import org.junit.Assert.assertNull
import org.junit.Test

/**
 * The wire format is hand-encoded, so nothing else checks it. A frame the
 * receiver cannot parse produces silence rather than an error — it simply
 * ignores the message — which is exactly the failure that is impossible to
 * diagnose from the outside.
 */
class CastMessageTest {

	@Test
	fun `payload survives a round trip`() {
		val body = CastMessage.body(
			namespace = CastNs.MEDIA,
			source = CastNs.SENDER,
			destination = "transport-1",
			payload = """{"type":"PLAY","requestId":7}""",
		)
		assertEquals("""{"type":"PLAY","requestId":7}""", CastMessage.payloadOf(body))
	}

	/**
	 * The length prefix counts bytes, not characters. Getting this wrong only
	 * shows up with a non-ASCII track title, which is precisely the case a
	 * hand-written encoder gets wrong and nobody notices in testing.
	 */
	@Test
	fun `multi-byte characters round trip`() {
		val payload = """{"title":"Björk — Jóga","artist":"日本語"}"""
		val body = CastMessage.body(CastNs.MEDIA, CastNs.SENDER, "transport-1", payload)
		assertEquals(payload, CastMessage.payloadOf(body))
	}

	@Test
	fun `frame carries a big-endian length`() {
		val body = ByteArray(0x010203)
		val framed = CastMessage.frame(body)
		assertArrayEquals(byteArrayOf(0x00, 0x01, 0x02, 0x03), framed.copyOfRange(0, 4))
		assertEquals(4 + body.size, framed.size)
		assertEquals(body.size, CastMessage.frameLength(framed.copyOfRange(0, 4)))
	}

	/** A length above 32767 needs the high bit set correctly on both halves. */
	@Test
	fun `frame length reads back above the signed byte range`() {
		val header = byteArrayOf(0x00, 0x0f, 0xff.toByte(), 0xfe.toByte())
		assertEquals(0x0ffffe, CastMessage.frameLength(header))
	}

	/**
	 * Fields may arrive in any order and the payload is the last of six, so the
	 * decoder has to skip what it does not want by length rather than assume a
	 * layout.
	 */
	@Test
	fun `fields before the payload are skipped`() {
		val body = CastMessage.body(
			namespace = "urn:x-cast:some.very.long.namespace.that.pads.the.message",
			source = CastNs.SENDER,
			destination = "receiver-0",
			payload = "{}",
		)
		assertEquals("{}", CastMessage.payloadOf(body))
	}

	@Test
	fun `a message with no payload field reads as none`() {
		// Field 4 only: a namespace and nothing else.
		val body = byteArrayOf(0x22, 0x03, 'a'.code.toByte(), 'b'.code.toByte(), 'c'.code.toByte())
		assertNull(CastMessage.payloadOf(body))
	}

	@Test
	fun `a truncated field does not read past the end`() {
		// Field 6, length 40, but only three bytes follow.
		val body = byteArrayOf(0x32, 40, 'a'.code.toByte(), 'b'.code.toByte(), 'c'.code.toByte())
		assertNull(CastMessage.payloadOf(body))
	}

	@Test
	fun `an empty message reads as none`() {
		assertNull(CastMessage.payloadOf(ByteArray(0)))
	}
}
