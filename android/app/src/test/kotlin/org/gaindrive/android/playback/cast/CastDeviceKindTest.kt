package org.gaindrive.android.playback.cast

import org.junit.Assert.assertEquals
import org.junit.Test

/**
 * The classifier is a free function precisely so it can be tested: the `md`
 * record it reads is parsed inside a private extension on `NsdServiceInfo`,
 * which nothing can construct here.
 *
 * The WiiM strings below are the shapes their firmware and their own HTTP API
 * use. If a real device turns out to announce something else, add it here as
 * well as fixing the matcher — the point of the table is that the next make of
 * receiver is a row rather than an investigation.
 */
class CastDeviceKindTest {

	@Test
	fun `a wiim is recognised whatever its model line looks like`() {
		val models = listOf(
			"WiiM Pro",
			"WiiM Pro Plus",
			"WiiM Amp",
			"WiiM Mini",
			"WiiM Ultra",
			// The spelling their HTTP API's `project` field uses.
			"WiiM_AMP",
			// Nothing guarantees the case of a TXT record.
			"wiim pro",
			"Linkplay WiiM Pro",
		)
		for (model in models) {
			assertEquals(model, CastDeviceKind.WIIM, castDeviceKind(model))
		}
	}

	@Test
	fun `anything else is generic`() {
		val models = listOf(
			"Chromecast",
			"Chromecast Ultra",
			"Google Home Mini",
			"Nest Audio",
			"Bravia 4K VH2",
			"SHIELD Android TV",
		)
		for (model in models) {
			assertEquals(model, CastDeviceKind.GENERIC, castDeviceKind(model))
		}
	}

	/**
	 * A manually added device has no announcement to read, and a receiver may
	 * omit the record or send it empty. None of the three may be a WiiM.
	 */
	@Test
	fun `an absent model is generic rather than a crash`() {
		assertEquals(CastDeviceKind.GENERIC, castDeviceKind(null))
		assertEquals(CastDeviceKind.GENERIC, castDeviceKind(""))
		assertEquals(CastDeviceKind.GENERIC, castDeviceKind("   "))
	}

	@Test
	fun `the kind reads straight off a device`() {
		val wiim = CastDevice(id = "1", name = "Kitchen", address = "10.0.0.5", model = "WiiM Pro")
		val manual = CastDevice(id = "2", name = "Study", address = "10.0.0.6")
		assertEquals(CastDeviceKind.WIIM, wiim.kind)
		assertEquals(CastDeviceKind.GENERIC, manual.kind)
	}
}
