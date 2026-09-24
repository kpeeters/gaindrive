package org.gaindrive.android.playback.cast

import kotlinx.serialization.json.JsonArray
import kotlinx.serialization.json.JsonElement
import kotlinx.serialization.json.JsonObject
import kotlinx.serialization.json.JsonPrimitive
import kotlinx.serialization.json.booleanOrNull
import kotlinx.serialization.json.floatOrNull
import kotlinx.serialization.json.intOrNull

/**
 * A Chromecast on the network.
 *
 * [model] is the mDNS `md` record - the model name the receiver announces for
 * itself, `Chromecast` or `WiiM Pro` or a television's marketing name. It is
 * null for a manually added device, which has no announcement to read.
 *
 * Note that [CastSession.connect] early-returns on structural equality, so a
 * model that appeared and then vanished between resolves would read as a
 * different device and reconnect. `md` travels in the same TXT record as `fn`,
 * which has always carried that hazard for the name, so this adds no new one.
 */
data class CastDevice(
	val id: String,
	val name: String,
	val address: String,
	val port: Int = 8009,
	val model: String? = null,
)

/**
 * The receiver's device volume, 0..1, from a `RECEIVER_STATUS`.
 */
data class CastVolume(
	val level: Float,
	val muted: Boolean = false,
)

/**
 * What the receiver last said it was doing.
 *
 * [UNKNOWN] exists so a state we have never seen cannot be silently read as
 * IDLE, which is the one value that drives both the LOAD retry and the queue
 * advance.
 */
enum class CastPlayerState { IDLE, PLAYING, PAUSED, BUFFERING, LOADING, UNKNOWN }

/**
 * A parsed `MEDIA_STATUS`, mirroring `CastManager::CastStatus` in
 * `src/castmanager.cc`.
 *
 * [idleReason] is only meaningful while IDLE, and only two values matter here:
 * `FINISHED` advances the queue, `ERROR` triggers the LOAD retry.
 *
 * Every accessor below reaches into JSON that came off the network from a
 * device we do not control, so the whole file uses safe casts rather than
 * kotlinx's throwing `jsonObject`/`jsonArray` helpers - a malformed push must
 * not take down the receive loop.
 */
data class CastStatus(
	val playerState: CastPlayerState = CastPlayerState.IDLE,
	val currentTime: Float = 0f,
	val duration: Float = 0f,
	val mediaSessionId: Int = 0,
	val idleReason: String? = null,
	/**
	 * Which subtitle tracks the receiver has on, by trackId.
	 *
	 * **Nullable, and null is not empty.** The receiver states this when the
	 * selection changes and omits it from the position pushes in between,
	 * exactly as it does with `duration` - so reading an absent field as "none
	 * selected" would make the picker's tick, and the tinted CC icon, flicker
	 * off once a second. [CastSession] carries the last stated value forward.
	 */
	val activeTrackIds: List<Int>? = null,
) {
	val isIdleError: Boolean
		get() = playerState == CastPlayerState.IDLE && idleReason == "ERROR"

	val isIdleFinished: Boolean
		get() = playerState == CastPlayerState.IDLE && idleReason == "FINISHED"

	/** The receiver is doing something with our media, whatever it is. */
	val isLive: Boolean
		get() = playerState == CastPlayerState.PLAYING ||
			playerState == CastPlayerState.BUFFERING ||
			playerState == CastPlayerState.LOADING

	companion object {

		/**
		 * Reads the first entry of a `MEDIA_STATUS`, or null if the message
		 * carries no status at all.
		 *
		 * The receiver sends a list, but it holds one entry for the single-item
		 * sessions we load.
		 */
		fun parse(message: JsonObject): CastStatus? {
			val entry = message.array("status")?.firstOrNull() as? JsonObject
				?: return null

			// A push during playback omits `media` entirely - the receiver only
			// repeats it when the item changes - so a zero here means "not
			// stated", not "zero seconds". CastSession carries the last known
			// value forward.
			val duration = (entry.obj("media")?.get("duration") as? JsonPrimitive)
				?.floatOrNull
				?: 0f

			return CastStatus(
				playerState = when (entry.string("playerState")) {
					"IDLE", null -> CastPlayerState.IDLE
					"PLAYING" -> CastPlayerState.PLAYING
					"PAUSED" -> CastPlayerState.PAUSED
					"BUFFERING" -> CastPlayerState.BUFFERING
					"LOADING" -> CastPlayerState.LOADING
					else -> CastPlayerState.UNKNOWN
				},
				currentTime = (entry["currentTime"] as? JsonPrimitive)?.floatOrNull ?: 0f,
				duration = duration,
				mediaSessionId = (entry["mediaSessionId"] as? JsonPrimitive)?.intOrNull ?: 0,
				idleReason = entry.string("idleReason"),
				// Absent stays null - see the field. An empty array *is* a
				// statement, and it means the viewer turned subtitles off.
				activeTrackIds = entry.array("activeTrackIds")?.mapNotNull {
					(it as? JsonPrimitive)?.intOrNull
				},
			)
		}

		/**
		 * The `transportId` of a running instance of [appId], from a
		 * `RECEIVER_STATUS`. Null means it is not running, which is the signal to
		 * LAUNCH it.
		 */
		fun transportIdOf(message: JsonObject, appId: String): String? =
			message.runningApp(appId)?.string("transportId")

		/** The `sessionId` of that same application, needed to STOP it. */
		fun sessionIdOf(message: JsonObject, appId: String): String? =
			message.runningApp(appId)?.string("sessionId")

		/**
		 * Whether this `RECEIVER_STATUS` says what the television is running.
		 *
		 * The distinction [transportIdOf] cannot express: it answers null both
		 * for "our app is not running" and for "this status was not about
		 * applications at all". A volume-change push is the second - it carries
		 * `volume` and nothing else - and treating it as the first discards a
		 * transport that is still perfectly good.
		 */
		fun listsApplications(message: JsonObject): Boolean =
			message.obj("status")?.array("applications") != null

		/**
		 * The `volume` block of a `RECEIVER_STATUS`, or null when the message
		 * carries none. Absent means "not stated", never "silent": a status
		 * about applications alone must not zero the level, so callers keep
		 * their last value on null.
		 */
		fun volumeOf(message: JsonObject): CastVolume? {
			val volume = message.obj("status")?.obj("volume") ?: return null
			val level = (volume["level"] as? JsonPrimitive)?.floatOrNull ?: return null
			return CastVolume(
				level = level.coerceIn(0f, 1f),
				muted = (volume["muted"] as? JsonPrimitive)?.booleanOrNull ?: false,
			)
		}

		/**
		 * Matched on [appId] rather than taken as the first entry, which is a
		 * fix and not a refinement.
		 *
		 * A television that has been sitting idle is running its own ambient
		 * app - `E8C28D3C`, "Backdrop" - and it publishes a `transportId` like
		 * any other. Taking the first one made that look like a media receiver
		 * ready to be loaded into, so the LOAD went to a screensaver, which
		 * ignores the media namespace entirely: no `MEDIA_STATUS`, no fetch,
		 * nothing in any log, and a player that simply does nothing. Whether
		 * casting worked then depended on what the TV happened to be showing
		 * when the user reached for it.
		 */
		private fun JsonObject.runningApp(appId: String): JsonObject? =
			obj("status")?.array("applications")
				?.filterIsInstance<JsonObject>()
				?.firstOrNull { it.string("appId") == appId }

		private fun JsonObject.obj(key: String): JsonObject? = this[key] as? JsonObject

		private fun JsonObject.array(key: String): JsonArray? = this[key] as? JsonArray

		private fun JsonObject.string(key: String): String? =
			(this[key] as? JsonPrimitive)?.takeIf { it.isString }?.content?.takeIf { it.isNotEmpty() }

		/** Shared with [CastSession] for reading a message's `type`. */
		internal fun typeOf(message: JsonElement): String? =
			(message as? JsonObject)?.string("type")
	}
}
