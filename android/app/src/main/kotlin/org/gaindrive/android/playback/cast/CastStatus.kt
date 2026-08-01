package org.gaindrive.android.playback.cast

import kotlinx.serialization.json.JsonArray
import kotlinx.serialization.json.JsonElement
import kotlinx.serialization.json.JsonObject
import kotlinx.serialization.json.JsonPrimitive
import kotlinx.serialization.json.floatOrNull
import kotlinx.serialization.json.intOrNull

/** A Chromecast on the network. */
data class CastDevice(
	val id: String,
	val name: String,
	val address: String,
	val port: Int = 8009,
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
 * kotlinx's throwing `jsonObject`/`jsonArray` helpers — a malformed push must
 * not take down the receive loop.
 */
data class CastStatus(
	val playerState: CastPlayerState = CastPlayerState.IDLE,
	val currentTime: Float = 0f,
	val duration: Float = 0f,
	val mediaSessionId: Int = 0,
	val idleReason: String? = null,
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

			// A push during playback omits `media` entirely — the receiver only
			// repeats it when the item changes — so a zero here means "not
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
			)
		}

		/**
		 * The `transportId` of a running receiver application, from a
		 * `RECEIVER_STATUS`. Null means no application is running, which is the
		 * signal to LAUNCH one.
		 */
		fun transportIdOf(message: JsonObject): String? =
			message.runningApp()?.string("transportId")

		/** The `sessionId` of the running application, needed to STOP it. */
		fun sessionIdOf(message: JsonObject): String? =
			message.runningApp()?.string("sessionId")

		private fun JsonObject.runningApp(): JsonObject? =
			obj("status")?.array("applications")?.firstOrNull() as? JsonObject

		private fun JsonObject.obj(key: String): JsonObject? = this[key] as? JsonObject

		private fun JsonObject.array(key: String): JsonArray? = this[key] as? JsonArray

		private fun JsonObject.string(key: String): String? =
			(this[key] as? JsonPrimitive)?.takeIf { it.isString }?.content?.takeIf { it.isNotEmpty() }

		/** Shared with [CastSession] for reading a message's `type`. */
		internal fun typeOf(message: JsonElement): String? =
			(message as? JsonObject)?.string("type")
	}
}
