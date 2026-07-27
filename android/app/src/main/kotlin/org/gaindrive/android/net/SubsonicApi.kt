package org.gaindrive.android.net

import retrofit2.http.GET
import retrofit2.http.Query

/**
 * The Subsonic endpoints the app calls. Auth and protocol parameters are added
 * by [AuthInterceptor], so they never appear here.
 *
 * Phase 1 needs only enough to prove a server is reachable and to learn what
 * the account may do; Phase 2 adds the browsing endpoints.
 */
interface SubsonicApi {

	@GET("rest/ping.view")
	suspend fun ping(): SubsonicEnvelope<PingBody>

	@GET("rest/getUser.view")
	suspend fun getUser(@Query("username") username: String): SubsonicEnvelope<GetUserBody>
}
