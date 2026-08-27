package org.gaindrive.android.playback.wiim

import kotlinx.serialization.json.JsonArray
import kotlinx.serialization.json.JsonObject
import kotlinx.serialization.json.JsonPrimitive
import okhttp3.HttpUrl
import okhttp3.HttpUrl.Companion.toHttpUrl
import org.gaindrive.android.net.SubsonicJson

/**
 * The equalizer state a WiiM reports.
 *
 * [preset] is null when the device would not say which one is loaded — see
 * [parseEqStat]. That is a real state and not an error: the switch is still
 * worth showing.
 */
data class WiiMEqState(
	val enabled: Boolean,
	val preset: String?,
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
 * rule for a payload from a device we do not control — the same reasoning as
 * `CastStatus.kt` and `CastProbe`. A malformed body must yield null, never an
 * exception: some of these calls run from a detached coroutine.
 */

/** The command a WiiM device answers on. Port 443, and always HTTPS. */
private const val PATH = "httpapi.asp"

/**
 * Builds the URL for one command.
 *
 * **The query must be assembled by `HttpUrl`, never concatenated.** Four of the
 * documented presets contain a space and one — `R&B` — contains an ampersand,
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
 * The presets *HTTP API for WiiM Products v1.2* documents.
 *
 * Used only when `EQGetList` fails, so the sheet is never empty. It is not the
 * primary source because firmware versions differ and newer ones add presets —
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
 * selection is barely a picker. The band values are ignored — a graphic EQ is
 * not what this screen offers.
 */
fun parseEqBand(body: String): WiiMEqState? {
	val root = asObject(body) ?: return null
	val stat = root.string("EQStat") ?: return null
	return WiiMEqState(enabled = stat.equals("on", ignoreCase = true), preset = root.string("Name"))
}

/**
 * Reads an `EQGetStat` response — `{"EQStat":"On"}` or `{"EQStat":"Off"}`.
 *
 * The fallback. `EQGetBand` is documented by a third-party project rather than
 * by WiiM's own PDF, so a firmware without it is entirely possible; `EQGetStat`
 * is in the PDF. It costs the preset name, which is why it is second.
 */
fun parseEqStat(body: String): Boolean? =
	asObject(body)?.string("EQStat")?.equals("on", ignoreCase = true)

/**
 * Reads an `EQGetList` response: a bare JSON array of names.
 *
 * Served as `text/html` despite being JSON, which is why nothing here consults
 * the content type. An empty array is treated as a miss — a device with no
 * presets at all is not a state worth rendering, and the documented list is a
 * better answer than a blank sheet.
 */
fun parsePresets(body: String): List<String>? {
	val root = runCatching { SubsonicJson.parseToJsonElement(body) }.getOrNull() as? JsonArray
		?: return null
	val names = root.mapNotNull { (it as? JsonPrimitive)?.takeIf { p -> p.isString }?.content }
		.filter { it.isNotBlank() }
	return names.ifEmpty { null }
}

/**
 * Whether a command that answers `OK` or `Failed` succeeded.
 *
 * The response is plain text, despite the OpenAPI description typing it as an
 * object with a `status` field. Trimmed because a trailing newline is ordinary.
 */
fun isOk(body: String): Boolean = body.trim().equals("OK", ignoreCase = true)

private fun asObject(body: String): JsonObject? =
	runCatching { SubsonicJson.parseToJsonElement(body) }.getOrNull() as? JsonObject

/** Safe by construction, like the rest of the foreign-JSON reading. */
private fun JsonObject.string(key: String): String? =
	(this[key] as? JsonPrimitive)?.takeIf { it.isString }?.content?.takeIf { it.isNotBlank() }
