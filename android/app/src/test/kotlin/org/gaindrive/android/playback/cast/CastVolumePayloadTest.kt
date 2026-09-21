package org.gaindrive.android.playback.cast

import org.junit.Assert.assertEquals
import org.junit.Test

/**
 * The SET_VOLUME wire shape. A payload the receiver cannot parse produces
 * silence rather than an error, CastMessageTest's reasoning exactly.
 */
class CastVolumePayloadTest {

	@Test
	fun `the payload carries the level inside a volume object`() {
		assertEquals(
			"""{"type":"SET_VOLUME","requestId":7,"volume":{"level":0.5}}""",
			setVolumePayload(7, 0.5f).toString(),
		)
	}
}
