package org.gaindrive.android.net

import kotlinx.serialization.SerialName
import kotlinx.serialization.Serializable

/**
 * Every Subsonic response is `{"subsonic-response": {...}}`. The payload is a
 * sibling of `status`/`error` rather than nested under a key of its own, so the
 * generic parameter is the whole body rather than just the payload.
 */
@Serializable
data class SubsonicEnvelope<T>(
	@SerialName("subsonic-response") val response: T,
)

/** Fields every response body carries, whatever the endpoint. */
interface SubsonicBody {
	val status: String
	val error: SubsonicError?
}

@Serializable
data class SubsonicError(
	val code: Int = 0,
	val message: String = "",
)

@Serializable
data class PingBody(
	override val status: String = "failed",
	val version: String? = null,
	@SerialName("serverVersion") val serverVersion: String? = null,
	override val error: SubsonicError? = null,
) : SubsonicBody

@Serializable
data class GetUserBody(
	override val status: String = "failed",
	override val error: SubsonicError? = null,
	val user: UserDto? = null,
) : SubsonicBody

/**
 * Roles are per server: the same person can be an admin on one and a
 * restricted account on another. `castRole` is deliberately absent - it gates
 * the server-driven cast endpoints, which this app does not use.
 */
@Serializable
data class UserDto(
	val username: String = "",
	val email: String? = null,
	val adminRole: Boolean = false,
	val uploadRole: Boolean = false,
	val downloadRole: Boolean = false,
	val maxBitRate: Int = 0,
)
