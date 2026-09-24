package org.gaindrive.android.playback.cast

import kotlinx.serialization.json.Json
import kotlinx.serialization.json.JsonObject
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Test

/**
 * Status parsing reads JSON from a device we do not control, and every field is
 * optional in practice. The payloads below are shaped like real ones - see the
 * `Cast rx MEDIA_STATUS payload` lines the server logs.
 */
class CastStatusTest {

	/** The Default Media Receiver, the only app these lookups are about. */
	private val MEDIA_APP = "CC1AD845"

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
	 * reads as zero. It is [CastSession]'s job to carry the old value forward -
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
		assertEquals("transport-9", CastStatus.transportIdOf(message(raw), MEDIA_APP))
		assertEquals("a-session", CastStatus.sessionIdOf(message(raw), MEDIA_APP))
	}

	/**
	 * No running application is the signal to LAUNCH one, so it has to be
	 * distinguishable from a status we failed to read.
	 */
	@Test
	fun `no running app yields no transport`() {
		assertNull(CastStatus.transportIdOf(message("""{"status":{"applications":[]}}"""), MEDIA_APP))
		assertNull(CastStatus.transportIdOf(message("""{"status":{}}"""), MEDIA_APP))
		assertNull(
			CastStatus.transportIdOf(
				message("""{"status":{"applications":[{"appId":"CC1AD845","transportId":""}]}}"""),
				MEDIA_APP,
			)
		)
	}

	/**
	 * The regression that made casting depend on what the television happened
	 * to be showing: an idle TV runs its own ambient app, which publishes a
	 * transportId like any other, and loading into it does nothing at all.
	 */
	@Test
	fun `another app running is not a media receiver`() {
		val backdrop = """
			{"type":"RECEIVER_STATUS","status":{"applications":[{
			  "appId":"E8C28D3C","isIdleScreen":true,
			  "sessionId":"ad7da4be","transportId":"ad7da4be"
			}]}}
		""".trimIndent()
		assertNull(CastStatus.transportIdOf(message(backdrop), MEDIA_APP))
		assertNull(CastStatus.sessionIdOf(message(backdrop), MEDIA_APP))
	}

	/** And it is found when it is one of several. */
	@Test
	fun `the media receiver is picked out of a list`() {
		val both = """
			{"type":"RECEIVER_STATUS","status":{"applications":[
			  {"appId":"E8C28D3C","transportId":"backdrop"},
			  {"appId":"CC1AD845","transportId":"ours"}
			]}}
		""".trimIndent()
		assertEquals("ours", CastStatus.transportIdOf(message(both), MEDIA_APP))
	}

	/**
	 * The distinction `transportIdOf` cannot make on its own: it answers null
	 * both for "our app is not running" and for "this status was not about
	 * applications at all". Only the first means the cached transport is stale.
	 *
	 * A volume push is the case that matters - it arrives during ordinary
	 * playback, and reading it as an app teardown threw away a working
	 * transport, pushing the next load onto the slow LAUNCH path.
	 */
	@Test
	fun `a status that names no applications is not an empty list of them`() {
		assertTrue(CastStatus.listsApplications(message("""{"status":{"applications":[]}}""")))
		assertTrue(
			CastStatus.listsApplications(
				message("""{"status":{"applications":[{"appId":"E8C28D3C"}]}}""")
			)
		)

		assertFalse(CastStatus.listsApplications(message("""{"status":{"volume":{"level":0.4}}}""")))
		assertFalse(CastStatus.listsApplications(message("""{"status":{}}""")))
		assertFalse(CastStatus.listsApplications(message("""{"type":"RECEIVER_STATUS"}""")))
	}

	@Test
	fun `volume is read from a full status and from a volume-only push`() {
		val full = """
			{"type":"RECEIVER_STATUS","status":{
			  "applications":[{"appId":"CC1AD845","transportId":"ours"}],
			  "volume":{"level":0.7,"muted":false}
			}}
		""".trimIndent()
		assertEquals(CastVolume(0.7f), CastStatus.volumeOf(message(full)))

		// The push that carries volume and nothing else. It must feed the
		// indicator and still not count as an application listing.
		val push = """{"type":"RECEIVER_STATUS","status":{"volume":{"level":0.35,"muted":true}}}"""
		assertEquals(CastVolume(0.35f, muted = true), CastStatus.volumeOf(message(push)))
		assertFalse(CastStatus.listsApplications(message(push)))
	}

	/** Absent means "not stated"; callers keep their last value on null. */
	@Test
	fun `a status without a volume block reports none`() {
		assertNull(CastStatus.volumeOf(message("""{"status":{"applications":[]}}""")))
		assertNull(CastStatus.volumeOf(message("""{"status":{"volume":{"muted":true}}}""")))
		assertNull(CastStatus.volumeOf(message("""{"type":"RECEIVER_STATUS"}""")))
	}

	@Test
	fun `an out-of-range level is clamped`() {
		assertEquals(CastVolume(1f), CastStatus.volumeOf(message("""{"status":{"volume":{"level":1.4}}}""")))
		assertEquals(CastVolume(0f), CastStatus.volumeOf(message("""{"status":{"volume":{"level":-0.1}}}""")))
	}
}
