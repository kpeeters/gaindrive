package org.gaindrive.android.playback.wiim

import kotlinx.serialization.json.Json
import kotlinx.serialization.json.JsonArray
import kotlinx.serialization.json.JsonObject
import kotlinx.serialization.json.int
import kotlinx.serialization.json.jsonPrimitive
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertNull
import org.junit.Assert.assertThrows
import org.junit.Assert.assertTrue
import org.junit.Test

/**
 * Everything a WiiM answers with comes off the network from a device we do not
 * control, so the rule these tests exist to hold is that a surprise yields null
 * rather than an exception - some of these calls run from a coroutine whose
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
		// Two of ten bands is no fader row at all: bands stays null while the
		// switch and the preset still parse.
		assertEquals(WiiMEqState(enabled = false, preset = "Rock", bands = null), state)
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
	 * failure - the sheet falls back to the documented list and looks complete -
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

	@Test
	fun `a full band array is read in band order`() {
		val values = listOf(71, 67, 50, 45, 50, 55, 60, 50, 48, 52)
		val state = parseEqBand(tenBandBody(values.map { it.toString() }))
		assertEquals(values, state?.bands)
		assertEquals("Rock", state?.preset)
	}

	/** The index fields are decoration; the names are what binds a value. */
	@Test
	fun `band order comes from the names, not the index fields`() {
		val reversed = (9 downTo 0).joinToString(",") { i ->
			"""{"index":$i,"param_name":"${WIIM_BANDS[i].first}","value":${10 * i}}"""
		}
		val state = parseEqBand("""{"EQStat":"On","EQBand":[$reversed]}""")
		assertEquals((0..9).map { it * 10 }, state?.bands)
	}

	@Test
	fun `an unknown extra band is ignored`() {
		val extra = tenBandBody(List(10) { "50" }).replace(
			"]}",
			""",{"index":10,"param_name":"band32khz","value":42}]}""",
		)
		assertEquals(List(10) { 50 }, parseEqBand(extra)?.bands)
	}

	@Test
	fun `an unreadable band value withholds the faders, not the state`() {
		val values = MutableList(10) { "50" }
		values[3] = "\"loud\""
		val state = parseEqBand(tenBandBody(values))
		assertEquals(WiiMEqState(enabled = true, preset = "Rock", bands = null), state)
	}

	/** The harmless reading of a value off the documented scale. */
	@Test
	fun `band values are clamped to the device scale`() {
		val values = MutableList(10) { "50" }
		values[0] = "-20"
		values[9] = "150"
		val bands = parseEqBand(tenBandBody(values))?.bands
		assertEquals(WIIM_LEVEL_MIN, bands?.first())
		assertEquals(WIIM_LEVEL_MAX, bands?.last())
	}

	/**
	 * Parsed back rather than pinned as a string: the property is that a device
	 * reading the JSON sees ten correctly named bands, not that kotlinx spells
	 * the object in one particular order.
	 */
	@Test
	fun `EQSetBand carries all ten bands in the device's shape`() {
		val levels = (0 until 10).map { 40 + it }
		val command = eqSetBandCommand(levels)
		assertTrue(command, command.startsWith("EQSetBand:"))
		val root = Json.parseToJsonElement(command.removePrefix("EQSetBand:")) as JsonObject
		val bands = root["EQBand"] as JsonArray
		assertEquals(10, bands.size)
		bands.forEachIndexed { index, element ->
			val band = element as JsonObject
			assertEquals(index, band["index"]?.jsonPrimitive?.int)
			assertEquals(WIIM_BANDS[index].first, band["param_name"]?.jsonPrimitive?.content)
			assertEquals(40 + index, band["value"]?.jsonPrimitive?.int)
		}
	}

	/** A wrong-sized curve is a programming error, not a device condition. */
	@Test
	fun `EQSetBand refuses a curve that is not ten bands`() {
		assertThrows(IllegalArgumentException::class.java) { eqSetBandCommand(listOf(50)) }
		assertThrows(IllegalArgumentException::class.java) { eqSetBandCommand(List(11) { 50 }) }
	}

	/** The braces-and-quotes analogue of the `R&B` test below. */
	@Test
	fun `the band command survives the query encoding whole`() {
		val command = eqSetBandCommand(List(10) { 50 })
		val url = wiimUrl("10.0.0.5", command)
		assertEquals(url.toString(), 1, url.querySize)
		assertFalse(url.toString(), url.encodedQuery!!.contains("&"))
		assertFalse(url.toString(), url.encodedQuery!!.contains("\""))
		assertEquals(command, url.queryParameter("command"))
	}

	/**
	 * The one that would break in the field rather than here.
	 *
	 * `R&B` unencoded ends the `command` parameter and delivers the device
	 * `EQLoad:R`, and four documented presets carry a space. Both are why the
	 * URL is built by `HttpUrl` and never concatenated.
	 *
	 * Asserted as the failure mode - one parameter, no raw `&` - rather than as
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
	/** The measured `EQGetBand` shape with a full band array. */
	private fun tenBandBody(values: List<String>, name: String = "Rock"): String {
		val bands = values.mapIndexed { i, v ->
			"""{"index":$i,"param_name":"${WIIM_BANDS[i].first}","value":$v}"""
		}.joinToString(",")
		return """{"status":"OK","source_name":"wifi","EQStat":"On","Name":"$name","EQBand":[$bands]}"""
	}
}
