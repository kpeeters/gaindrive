package org.gaindrive.android.playback.wiim

import kotlinx.serialization.json.JsonArray
import kotlinx.serialization.json.JsonObject
import kotlinx.serialization.json.JsonPrimitive
import kotlinx.serialization.json.buildJsonArray
import kotlinx.serialization.json.buildJsonObject
import kotlinx.serialization.json.intOrNull
import kotlinx.serialization.json.put
import okhttp3.HttpUrl
import okhttp3.HttpUrl.Companion.toHttpUrl
import org.gaindrive.android.net.SubsonicJson

/**
 * The equalizer state a WiiM reports.
 *
 * [preset] is null when the device would not say which one is loaded - see
 * [parseEqStat]. That is a real state and not an error: the switch is still
 * worth showing.
 */
data class WiiMEqState(
	val enabled: Boolean,
	val preset: String?,
	/**
	 * Fader positions in [WIIM_BANDS] order, or null when the device gave no
	 * usable set (the `EQGetStat` fallback, or a band missing or unreadable).
	 * Null is a real state too: the sheet then degrades to the switch and the
	 * preset list instead of drawing faders it could not fill.
	 */
	val bands: List<Int>? = null,
)

/**
 * Everything about the WiiM HTTP API that can be decided without a socket:
 * building a command URL and reading a response. `WiiMClient` is the I/O around
 * it.
 *
 * Split out for the reason `CastDeviceKind` and `LoadRetryWatcher` are: this
 * project has no ViewModel tests and no `MainDispatcherRule`, so what is worth
 * testing goes in a pure function. Everything here is exercised by `WiiMEqTest`.
 *
 * Bodies are read with safe casts rather than `@Serializable` DTOs, the house
 * rule for a payload from a device we do not control - the same reasoning as
 * `CastStatus.kt` and `CastProbe`. A malformed body must yield null, never an
 * exception: some of these calls run from a detached coroutine.
 */

/** The command a WiiM device answers on. Port 443, and always HTTPS. */
private const val PATH = "httpapi.asp"

/**
 * Builds the URL for one command.
 *
 * **The query must be assembled by `HttpUrl`, never concatenated.** Four of the
 * documented presets contain a space and one - `R&B` - contains an ampersand,
 * which unencoded would start a second query parameter and deliver the device
 * `EQLoad:R`. `addQueryParameter` percent-encodes both.
 *
 * Note the port: 443, not `CastDevice.port`. That field is 8009, the Cast v2
 * control port, and has nothing to do with this API.
 */
fun wiimUrl(address: String, command: String): HttpUrl =
	"https://$address/$PATH".toHttpUrl()
		.newBuilder()
		.addQueryParameter("command", command)
		.build()

/** The commands this app sends. `EQGetStat` is the fallback for [EQ_GET_BAND]. */
const val EQ_GET_BAND = "EQGetBand"
const val EQ_GET_STAT = "EQGetStat"
const val EQ_GET_LIST = "EQGetList"
const val EQ_ON = "EQOn"
const val EQ_OFF = "EQOff"

fun eqLoadCommand(preset: String): String = "EQLoad:$preset"

/**
 * The graphic EQ's ten fixed bands: `param_name` to fader label, in device
 * index order. Fixed rather than read from the device, unlike the preset list,
 * because `EQSetBand` must name them and a band this table does not know could
 * not be written anyway.
 */
val WIIM_BANDS: List<Pair<String, String>> = listOf(
	"band31hz" to "31",
	"band63hz" to "63",
	"band125hz" to "125",
	"band250hz" to "250",
	"band500hz" to "500",
	"band1khz" to "1k",
	"band2khz" to "2k",
	"band4khz" to "4k",
	"band8khz" to "8k",
	"band16khz" to "16k",
)

/**
 * The device's fader scale. How 0..99 maps to decibels is unverified (the WiiM
 * app draws +-12 dB), which is why the sheet shows offsets from flat and not
 * dB figures it cannot vouch for.
 */
const val WIIM_LEVEL_MIN = 0
const val WIIM_LEVEL_MAX = 99
const val WIIM_LEVEL_FLAT = 50

/**
 * The presets *HTTP API for WiiM Products v1.2* documents.
 *
 * Used only when `EQGetList` fails, so the sheet is never empty. It is not the
 * primary source because firmware versions differ and newer ones add presets -
 * a hardcoded list would silently hide them.
 */
val DOCUMENTED_PRESETS: List<String> = listOf(
	"Flat", "Acoustic", "Bass Booster", "Bass Reducer", "Classical", "Dance",
	"Deep", "Electronic", "Hip-Hop", "Jazz", "Latin", "Loudness", "Lounge",
	"Piano", "Pop", "R&B", "Rock", "Small Speakers", "Spoken Word",
	"Treble Booster", "Treble Reducer", "Vocal Booster",
)

/**
 * Reads an `EQGetBand` response, which carries the whole state at once:
 *
 *     {"status":"OK","source_name":"wifi","EQStat":"Off","Name":"Rock",
 *      "EQBand":[{"index":0,"param_name":"band31hz","value":71}, …]}
 *
 * This is why `EQGetBand` is the state read rather than `EQGetStat`: only this
 * one says *which* preset is loaded, and a picker that cannot show the current
 * selection is barely a picker. The band values feed the fader row; a body
 * without a usable set still yields the switch and the preset.
 */
fun parseEqBand(body: String): WiiMEqState? {
	val root = asObject(body) ?: return null
	val stat = root.string("EQStat") ?: return null
	return WiiMEqState(
		enabled = stat.equals("on", ignoreCase = true),
		preset = root.string("Name"),
		bands = parseBands(root),
	)
}

/**
 * The `EQBand` array as fader positions in [WIIM_BANDS] order.
 *
 * Matched by `param_name` rather than by the `index` field, so a reordered
 * array still reads correctly. All ten or nothing: a fader row with a hole in
 * it has no honest rendering, and half a curve written back would be a curve
 * the user never shaped.
 */
private fun parseBands(root: JsonObject): List<Int>? {
	val entries = (root["EQBand"] as? JsonArray)?.filterIsInstance<JsonObject>() ?: return null
	val byName = entries.mapNotNull { entry ->
		val name = entry.string("param_name") ?: return@mapNotNull null
		val value = (entry["value"] as? JsonPrimitive)?.intOrNull ?: return@mapNotNull null
		name to value
	}.toMap()
	return WIIM_BANDS.map { (name, _) ->
		(byName[name] ?: return null).coerceIn(WIIM_LEVEL_MIN, WIIM_LEVEL_MAX)
	}
}

/**
 * Reads an `EQGetStat` response - `{"EQStat":"On"}` or `{"EQStat":"Off"}`.
 *
 * The fallback. `EQGetBand` is documented by a third-party project rather than
 * by WiiM's own PDF, so a firmware without it is entirely possible; `EQGetStat`
 * is in the PDF. It costs the preset name, which is why it is second.
 */
fun parseEqStat(body: String): Boolean? =
	asObject(body)?.string("EQStat")?.equals("on", ignoreCase = true)

/**
 * Reads an `EQGetList` response: a JSON array of names, bare or wrapped.
 *
 * Served as `text/html` despite being JSON, which is why nothing here consults
 * the content type. An empty array is treated as a miss - a device with no
 * presets at all is not a state worth rendering, and the documented list is a
 * better answer than a blank sheet.
 *
 * **An array inside an object counts**, though no device has been seen sending
 * one: the firmware measured here answers this command with a bare array while
 * wrapping `EQGetBand` and every mutation in `{"status":"OK", …}`, so the shape
 * is plainly within its repertoire. Accepting both costs three lines, and the
 * consequence of refusing is silent - `presets()` never throws, so an unread
 * list is indistinguishable on screen from a device offering exactly the
 * documented set, and the difference only surfaces as a preset the owner made
 * themselves being absent. The field is found by shape and not by name because
 * nothing documents the name, and no such body carries a second array.
 */
fun parsePresets(body: String): List<String>? {
	val root = runCatching { SubsonicJson.parseToJsonElement(body) }.getOrNull() ?: return null
	val array = root as? JsonArray
		?: (root as? JsonObject)?.values?.firstNotNullOfOrNull { it as? JsonArray }
		?: return null
	val names = array.mapNotNull { (it as? JsonPrimitive)?.takeIf { p -> p.isString }?.content }
		.filter { it.isNotBlank() }
	return names.ifEmpty { null }
}

/**
 * The command that writes all ten fader positions at once.
 *
 * Community-documented like `EQGetBand`, not in WiiM's own PDF; `WIIM.md`
 * records what is verified. Always the whole curve: the device accepts a
 * partial array, but sending one would make the outcome depend on state this
 * app last read rather than on what the user sees. Encoding the braces and
 * quotes into the URL is [wiimUrl]'s job, as everywhere else.
 */
fun eqSetBandCommand(levels: List<Int>): String {
	require(levels.size == WIIM_BANDS.size) {
		"expected ${WIIM_BANDS.size} band levels, got ${levels.size}"
	}
	val payload = buildJsonObject {
		put("EQBand", buildJsonArray {
			levels.forEachIndexed { index, value ->
				add(buildJsonObject {
					put("index", index)
					put("param_name", WIIM_BANDS[index].first)
					put("value", value.coerceIn(WIIM_LEVEL_MIN, WIIM_LEVEL_MAX))
				})
			}
		})
	}
	return "EQSetBand:$payload"
}

/**
 * Whether a command that answers `OK` or `Failed` succeeded.
 *
 * **Both documented shapes are accepted, because a device sends the other one.**
 * The PDF describes a plain-text `OK`, the OpenAPI description types the
 * response as an object with a `status` field, and this code believed the PDF -
 * so on real firmware every `EQLoad`, `EQOn` and `EQOff` was performed by the
 * device and reported to the user as "the device would not load that preset".
 * A command that is obeyed and then called a failure is the worst of the three
 * possible answers, so where the two documents disagree, accept both.
 *
 * `Failed` in either shape stays a failure: this is not "any 2xx is success".
 * Trimmed because a trailing newline is ordinary from this device.
 */
fun isOk(body: String): Boolean {
	val text = body.trim()
	if (text.equals("OK", ignoreCase = true)) return true
	return asObject(text)?.string("status")?.equals("OK", ignoreCase = true) == true
}

private fun asObject(body: String): JsonObject? =
	runCatching { SubsonicJson.parseToJsonElement(body) }.getOrNull() as? JsonObject

/** Safe by construction, like the rest of the foreign-JSON reading. */
private fun JsonObject.string(key: String): String? =
	(this[key] as? JsonPrimitive)?.takeIf { it.isString }?.content?.takeIf { it.isNotBlank() }
