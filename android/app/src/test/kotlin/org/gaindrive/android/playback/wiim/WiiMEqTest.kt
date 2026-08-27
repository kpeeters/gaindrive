package org.gaindrive.android.playback.wiim

import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Test

/**
 * Everything a WiiM answers with comes off the network from a device we do not
 * control, so the rule these tests exist to hold is that a surprise yields null
 * rather than an exception — some of these calls run from a coroutine whose
 * failure would only surface as a blank sheet.
 *
 * The bodies below are the ones quoted in the WiiM HTTP API documentation.
 */
class WiiMEqTest {

	private val eqGetBand = """
		{"status":"OK","source_name":"wifi","EQStat":"Off","Name":"Rock",
		 "pluginURI":"http://moddevices.com/plugins/caps/Eq10HP",
		 "channelMode":"Stereo",
		 "EQBand":[{"index":0,"param_name":"band31hz","value":71},
		           {"index":1,"param_name":"band63hz","value":67}]}
	""".trimIndent()

	@Test
	fun `EQGetBand yields both the switch and the preset`() {
		val state = parseEqBand(eqGetBand)
		assertEquals(WiiMEqState(enabled = false, preset = "Rock"), state)
	}

	@Test
	fun `an EQGetBand with the equalizer on is read as on`() {
		val state = parseEqBand("""{"status":"OK","EQStat":"On","Name":"Jazz"}""")
		assertEquals(WiiMEqState(enabled = true, preset = "Jazz"), state)
	}

	/**
	 * The reason `EQGetBand` is preferred and `EQGetStat` is only the fallback:
	 * this one cannot say which preset is loaded, so the picker has nothing to
	 * tick.
	 */
	@Test
	fun `EQGetStat gives the switch and nothing else`() {
		assertEquals(true, parseEqStat("""{"EQStat":"On"}"""))
		assertEquals(false, parseEqStat("""{"EQStat":"Off"}"""))
	}

	@Test
	fun `a band response with no name still gives a state`() {
		assertEquals(WiiMEqState(enabled = true, preset = null), parseEqBand("""{"EQStat":"On"}"""))
	}

	@Test
	fun `the preset list is read as an array of names`() {
		val presets = parsePresets("""["Flat", "Acoustic", "Bass Booster", "R&B"]""")
		assertEquals(listOf("Flat", "Acoustic", "Bass Booster", "R&B"), presets)
	}

	/**
	 * Defensive rather than measured: the device sends a bare array here, but
	 * wraps every other response. Refusing a wrapped one is not a visible
	 * failure — the sheet falls back to the documented list and looks complete —
	 * so it would cost only the presets the owner made themselves, which are the
	 * ones they are looking for.
	 */
	@Test
	fun `a list wrapped in a status object is still a list`() {
		val presets = parsePresets(
			"""{"status":"OK","EQList":["Flat","Rock","LotsOfHigh"]}"""
		)
		assertEquals(listOf("Flat", "Rock", "LotsOfHigh"), presets)
	}

	/** A device offering nothing is not worth drawing; the documented list wins. */
	@Test
	fun `an empty preset list is a miss`() {
		assertNull(parsePresets("[]"))
		assertNull(parsePresets("""["", "  "]"""))
		assertNull(parsePresets("""{"status":"OK","EQList":[]}"""))
		// An object carrying no array at all is still a miss, not an empty list.
		assertNull(parsePresets("""{"status":"Failed"}"""))
	}

	@Test
	fun `OK and Failed are told apart`() {
		assertTrue(isOk("OK"))
		// A trailing newline is ordinary from this device.
		assertTrue(isOk("OK\n"))
		assertFalse(isOk("Failed"))
		assertFalse(isOk(""))
	}

	/**
	 * Measured against hardware: the device performs the command and answers in
	 * the OpenAPI description's shape, not the PDF's plain text. Reading only the
	 * latter reported every successful `EQLoad`/`EQOn`/`EQOff` as a refusal.
	 */
	@Test
	fun `an OK wrapped in an object is still OK`() {
		assertTrue(isOk("""{"status":"OK"}"""))
		assertTrue(isOk("""{"status":"OK"}""" + "\n"))
		assertTrue(isOk("""{"status":"ok","EQStat":"On"}"""))
	}

	/** Tolerating the second shape must not turn every 2xx into a success. */
	@Test
	fun `a wrapped failure is still a failure`() {
		assertFalse(isOk("""{"status":"Failed"}"""))
		assertFalse(isOk("""{"EQStat":"On"}"""))
		assertFalse(isOk("""{"status":""}"""))
		assertFalse(isOk("""{"status":7}"""))
		assertFalse(isOk("<html>404</html>"))
	}

	@Test
	fun `nothing throws on a body that is not what was expected`() {
		val nonsense = listOf("", "   ", "<html>404</html>", "{", "null", "42", """{"EQStat":7}""")
		for (body in nonsense) {
			assertNull(body, parseEqBand(body))
			assertNull(body, parseEqStat(body))
			assertNull(body, parsePresets(body))
		}
		// An array where an object belongs, and the reverse.
		assertNull(parseEqBand("""["Flat"]"""))
		assertNull(parsePresets("""{"EQStat":"On"}"""))
	}

	/**
	 * The one that would break in the field rather than here.
	 *
	 * `R&B` unencoded ends the `command` parameter and delivers the device
	 * `EQLoad:R`, and four documented presets carry a space. Both are why the
	 * URL is built by `HttpUrl` and never concatenated.
	 *
	 * Asserted as the failure mode — one parameter, no raw `&` — rather than as
	 * a literal query string. `addQueryParameter` encodes against OkHttp's query
	 * *component* set, which is deliberately wider than the reserved characters
	 * and includes `:`, so the command goes out as `EQLoad%3AR%26B`. Pinning that
	 * spelling ties the test to an implementation detail of OkHttp that says
	 * nothing about whether the device is served correctly.
	 */
	@Test
	fun `a preset name is percent-encoded into the query`() {
		val ampersand = wiimUrl("10.0.0.5", eqLoadCommand("R&B"))
		assertEquals(ampersand.toString(), 1, ampersand.querySize)
		assertFalse(ampersand.toString(), ampersand.encodedQuery!!.contains("&"))
		// Whatever the encoding, the device must receive the name back whole.
		assertEquals("EQLoad:R&B", ampersand.queryParameter("command"))

		val spaced = wiimUrl("10.0.0.5", eqLoadCommand("Small Speakers"))
		assertFalse(spaced.toString(), spaced.toString().contains(" "))
		assertEquals("EQLoad:Small Speakers", spaced.queryParameter("command"))
	}

	@Test
	fun `the URL is HTTPS on the default port, not the cast port`() {
		val url = wiimUrl("10.0.0.5", EQ_GET_BAND)
		assertEquals("https", url.scheme)
		assertEquals(443, url.port)
		assertEquals("/httpapi.asp", url.encodedPath)
	}

	/** A miscount here would silently hide presets from anyone on old firmware. */
	@Test
	fun `the documented fallback list is the twenty-two from the API document`() {
		assertEquals(22, DOCUMENTED_PRESETS.size)
		assertEquals("Flat", DOCUMENTED_PRESETS.first())
		assertTrue(DOCUMENTED_PRESETS.contains("R&B"))
		assertTrue(DOCUMENTED_PRESETS.contains("Small Speakers"))
	}
}
