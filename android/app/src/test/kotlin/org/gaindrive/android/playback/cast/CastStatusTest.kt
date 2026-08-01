package org.gaindrive.android.playback.cast

import kotlinx.serialization.json.Json
import kotlinx.serialization.json.JsonObject
import org.junit.Assert.assertEquals
import org.junit.Assert.assertNull
import org.junit.Test

/**
 * Status parsing reads JSON from a device we do not control, and every field is
 * optional in practice. The payloads below are shaped like real ones — see the
 * `Cast rx MEDIA_STATUS payload` lines the server logs.
 */
class CastStatusTest {

	private fun message(raw: String): JsonObject =
		Json.parseToJsonElement(raw) as JsonObject

	@Test
	fun `a playing status is read whole`() {
		val status = CastStatus.parse(
			message(
				"""
				{"type":"MEDIA_STATUS","status":[{
				  "mediaSessionId":6,
				  "playerState":"PLAYING",
				  "currentTime":41.5,
				  "media":{"contentId":"http://host/rest/stream.view","duration":213.7}
				}]}
				""".trimIndent()
			)
		)
		assertEquals(CastPlayerState.PLAYING, status?.playerState)
		assertEquals(41.5f, status?.currentTime)
		assertEquals(213.7f, status?.duration)
		assertEquals(6, status?.mediaSessionId)
		assertNull(status?.idleReason)
	}

	/**
	 * The receiver omits `media` on every push after the first, so duration
	 * reads as zero. It is [CastSession]'s job to carry the old value forward —
	 * this only pins down that the parser reports "not stated" rather than
	 * inventing something.
	 */
	@Test
	fun `a push without the media block reports no duration`() {
		val status = CastStatus.parse(
			message("""{"status":[{"mediaSessionId":6,"playerState":"PLAYING","currentTime":9}]}""")
		)
		assertEquals(0f, status?.duration)
		assertEquals(9f, status?.currentTime)
	}

	@Test
	fun `an idle error is recognised`() {
		val status = CastStatus.parse(
			message("""{"status":[{"mediaSessionId":6,"playerState":"IDLE","idleReason":"ERROR"}]}""")
		)!!
		assertEquals(true, status.isIdleError)
		assertEquals(false, status.isIdleFinished)
		assertEquals(false, status.isLive)
	}

	@Test
	fun `a finished track is recognised`() {
		val status = CastStatus.parse(
			message("""{"status":[{"mediaSessionId":6,"playerState":"IDLE","idleReason":"FINISHED"}]}""")
		)!!
		assertEquals(true, status.isIdleFinished)
		assertEquals(false, status.isIdleError)
	}

	/** An unfamiliar state must not be mistaken for IDLE, which drives retries. */
	@Test
	fun `an unknown player state stays unknown`() {
		val status = CastStatus.parse(
			message("""{"status":[{"mediaSessionId":6,"playerState":"SOMETHING_NEW"}]}""")
		)
		assertEquals(CastPlayerState.UNKNOWN, status?.playerState)
	}

	@Test
	fun `an empty or absent status list parses to nothing`() {
		assertNull(CastStatus.parse(message("""{"type":"MEDIA_STATUS","status":[]}""")))
		assertNull(CastStatus.parse(message("""{"type":"MEDIA_STATUS"}""")))
		assertNull(CastStatus.parse(message("""{"status":"nonsense"}""")))
	}

	@Test
	fun `the transport of a running app is found`() {
		val raw = """
			{"type":"RECEIVER_STATUS","status":{"applications":[{
			  "appId":"CC1AD845","sessionId":"a-session","transportId":"transport-9"
			}]}}
		""".trimIndent()
		assertEquals("transport-9", CastStatus.transportIdOf(message(raw)))
		assertEquals("a-session", CastStatus.sessionIdOf(message(raw)))
	}

	/**
	 * No running application is the signal to LAUNCH one, so it has to be
	 * distinguishable from a status we failed to read.
	 */
	@Test
	fun `no running app yields no transport`() {
		assertNull(CastStatus.transportIdOf(message("""{"status":{"applications":[]}}""")))
		assertNull(CastStatus.transportIdOf(message("""{"status":{}}""")))
		assertNull(
			CastStatus.transportIdOf(
				message("""{"status":{"applications":[{"appId":"CC1AD845","transportId":""}]}}""")
			)
		)
	}
}
