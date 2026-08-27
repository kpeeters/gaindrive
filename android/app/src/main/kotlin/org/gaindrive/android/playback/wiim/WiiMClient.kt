package org.gaindrive.android.playback.wiim

import android.annotation.SuppressLint
import android.net.Network
import android.util.Log
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.withContext
import okhttp3.OkHttpClient
import okhttp3.Request
import org.gaindrive.android.net.runCatchingCancellable
import org.gaindrive.android.playback.cast.WifiNetworks
import java.io.IOException
import java.security.SecureRandom
import java.security.cert.X509Certificate
import java.util.concurrent.TimeUnit
import javax.inject.Inject
import javax.inject.Singleton
import javax.net.ssl.SSLContext
import javax.net.ssl.X509TrustManager

/**
 * Talks to a WiiM's own HTTP API — `https://<address>/httpapi.asp?command=…`.
 *
 * This is a second protocol to a device that merely also happens to be a cast
 * target, so it lives beside the cast package rather than inside it, and knows
 * nothing about sessions, queues or the music server. `WIIM.md` holds the
 * reasoning; `WiiMEq.kt` holds everything decidable without a socket.
 *
 * Failure is an exception rather than a null, so the caller can put
 * `Throwable.userMessage()` on screen — a handshake failure and an unplugged
 * speaker are different sentences and the user can act on the difference.
 */
@Singleton
class WiiMClient @Inject constructor(
	private val wifi: WifiNetworks,
) {

	private var boundNetwork: Network? = null
	private var boundClient: OkHttpClient? = null
	private val unboundClient: OkHttpClient by lazy { build(null) }

	/** The current EQ state, preferring the reading that names the preset. */
	suspend fun eqState(address: String): WiiMEqState = withContext(Dispatchers.IO) {
		val band = runCatchingCancellable { get(address, EQ_GET_BAND) }.getOrNull()
			?.let(::parseEqBand)
		if (band != null) return@withContext band
		// EQGetBand is documented by a third-party project rather than by WiiM's
		// own PDF, so a firmware without it is entirely plausible. EQGetStat is
		// in the PDF and costs only the preset name.
		Log.i(TAG, "$EQ_GET_BAND unusable at $address, falling back to $EQ_GET_STAT")
		val enabled = parseEqStat(get(address, EQ_GET_STAT))
			?: throw IOException("The device did not report its equalizer state.")
		WiiMEqState(enabled = enabled, preset = null)
	}

	/**
	 * The presets this device offers.
	 *
	 * Never throws: [eqState] runs first and is the reachability test, so a
	 * failure here means the list specifically, and the documented set is a
	 * better answer than an empty sheet.
	 */
	suspend fun presets(address: String): List<String> = withContext(Dispatchers.IO) {
		val listed = runCatchingCancellable { get(address, EQ_GET_LIST) }.getOrNull()
			?.let(::parsePresets)
		if (listed == null) Log.i(TAG, "$EQ_GET_LIST unusable at $address, using documented list")
		listed ?: DOCUMENTED_PRESETS
	}

	suspend fun loadPreset(address: String, preset: String): Boolean =
		withContext(Dispatchers.IO) { isOk(get(address, eqLoadCommand(preset))) }

	suspend fun setEqEnabled(address: String, enabled: Boolean): Boolean =
		withContext(Dispatchers.IO) { isOk(get(address, if (enabled) EQ_ON else EQ_OFF)) }

	/**
	 * One command, tried Wi-Fi-bound and then unbound.
	 *
	 * That order is `CastChannel.connectPlain()`'s, and it is there for the same
	 * measured reason: binding to the Wi-Fi network is what reaches a LAN device
	 * under a full-tunnel VPN, but an ordinary `VpnService` that has not called
	 * `allowBypass()` — WireGuard does not — makes `Network.bindSocket` throw
	 * `EPERM` inside `createSocket()`, so the bound attempt cannot even begin.
	 * Unbound then works. `CastProbe.nameClient()` binds with no fallback and is
	 * deliberately *not* the model here: it is a nicety that may quietly fail,
	 * and this is a control API.
	 *
	 * A refusal is not a routing failure, so an HTTP response that is not 2xx
	 * ends the attempt rather than provoking a retry on the other client.
	 */
	private fun get(address: String, command: String): String {
		val request = Request.Builder().url(wiimUrl(address, command)).build()
		var failure: Throwable? = null
		for ((label, client) in clients()) {
			val attempt = runCatchingCancellable { execute(client, request) }
			attempt.getOrNull()?.let { return it }
			val error = attempt.exceptionOrNull() ?: continue
			Log.w(TAG, "$command to $address ($label, ${wifi.describe()}): ${error.message}")
			// A status came back, so the route works and the other client would
			// only ask the same question again and get the same answer.
			if (error is Refused) throw error
			failure = error
		}
		throw failure ?: IOException("The device did not answer.")
	}

	private fun execute(client: OkHttpClient, request: Request): String {
		client.newCall(request).execute().use { response ->
			if (!response.isSuccessful) throw Refused("The device answered HTTP ${response.code}.")
			return response.body?.string() ?: ""
		}
	}

	/** An answer, just not a usable one — as opposed to never reaching the device. */
	private class Refused(message: String) : IOException(message)

	/**
	 * The clients to try, in order. Built once and kept, unlike
	 * `CastProbe.nameClient()`: a control surface is called far more often than
	 * a probe, and each fresh client is another connection pool and dispatcher
	 * thread pool. The bound one is rebuilt when the phone joins a different
	 * Wi-Fi network, which is when [WifiNetworks.network] changes identity.
	 */
	@Synchronized
	private fun clients(): List<Pair<String, OkHttpClient>> {
		val network = wifi.network.value
		if (network != boundNetwork) {
			boundNetwork = network
			boundClient = network?.let(::build)
		}
		val bound = boundClient?.let { listOf("Wi-Fi-bound" to it) }.orEmpty()
		return bound + listOf("unbound" to unboundClient)
	}

	/**
	 * A bare client that trusts anything, scoped to this file and never made a
	 * default.
	 *
	 * WiiM present a self-signed LinkPlay certificate (`CN=www.linkplay.com`)
	 * that is in neither the system nor the user trust store, so ordinary
	 * verification cannot succeed — the same situation `CastChannel` is in with
	 * Chromecast device certificates, and it resolves it the same way. What
	 * authenticates the exchange is not the certificate: the user picked this
	 * device off their own network, and nothing secret is sent to it.
	 *
	 * The hostname verifier has to go too, which `CastChannel` never needed:
	 * it layers TLS over a raw socket and so never meets verification, whereas
	 * OkHttp checks a certificate naming `www.linkplay.com` against a bare IP
	 * and rejects it even with an all-trusting trust manager.
	 *
	 * Built here rather than from the injected `OkHttpClient` because the
	 * per-server copies of that carry `AuthInterceptor`, which appends the music
	 * account's `u=`/`t=`/`s=` to every request it sees. Those are credentials
	 * for the server and must never reach a speaker.
	 */
	@SuppressLint("CustomX509TrustManager", "TrustAllX509TrustManager", "BadHostnameVerifier")
	private fun build(network: Network?): OkHttpClient {
		val trustAny = object : X509TrustManager {
			override fun checkClientTrusted(chain: Array<X509Certificate>, authType: String) = Unit
			override fun checkServerTrusted(chain: Array<X509Certificate>, authType: String) = Unit
			override fun getAcceptedIssuers(): Array<X509Certificate> = emptyArray()
		}
		val context = SSLContext.getInstance("TLS").apply {
			init(null, arrayOf(trustAny), SecureRandom())
		}
		val builder = OkHttpClient.Builder()
			.connectTimeout(CONNECT_TIMEOUT_MS, TimeUnit.MILLISECONDS)
			.readTimeout(READ_TIMEOUT_MS, TimeUnit.MILLISECONDS)
			.sslSocketFactory(context.socketFactory, trustAny)
			.hostnameVerifier { _, _ -> true }
		network?.let { builder.socketFactory(it.socketFactory) }
		return builder.build()
	}

	private companion object {
		const val TAG = "GainDriveWiiM"

		/**
		 * Short, because the device is on the same LAN and every command here
		 * answers immediately. The bound attempt failing fast is the point: under
		 * a VPN it fails at socket creation, and the unbound retry is what the
		 * user is actually waiting for.
		 */
		const val CONNECT_TIMEOUT_MS = 3_000L
		const val READ_TIMEOUT_MS = 5_000L
	}
}
