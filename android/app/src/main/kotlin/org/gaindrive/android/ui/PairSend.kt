package org.gaindrive.android.ui

import android.util.Log
import android.widget.Toast
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.material3.AlertDialog
import androidx.compose.material3.CircularProgressIndicator
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Text
import androidx.compose.material3.TextButton
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.getValue
import androidx.compose.ui.Modifier
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.unit.dp
import androidx.hilt.lifecycle.viewmodel.compose.hiltViewModel
import androidx.lifecycle.ViewModel
import androidx.lifecycle.viewModelScope
import androidx.lifecycle.compose.collectAsStateWithLifecycle
import dagger.hilt.android.lifecycle.HiltViewModel
import java.io.IOException
import java.util.concurrent.TimeUnit
import javax.inject.Inject
import javax.net.SocketFactory
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.SharingStarted
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.asStateFlow
import kotlinx.coroutines.flow.first
import kotlinx.coroutines.flow.map
import kotlinx.coroutines.flow.stateIn
import kotlinx.coroutines.launch
import kotlinx.serialization.json.Json
import okhttp3.MediaType.Companion.toMediaType
import okhttp3.OkHttpClient
import okhttp3.Request
import okhttp3.RequestBody.Companion.toRequestBody
import org.gaindrive.android.data.PairLink
import org.gaindrive.android.data.PairPayload
import org.gaindrive.android.data.PairServer
import org.gaindrive.android.data.ServerRegistry
import org.gaindrive.android.data.crypto.PairingCipher
import org.gaindrive.android.data.model.ServerConfig
import org.gaindrive.android.playback.cast.WifiNetworks

/**
 * The phone side of QR pairing: the confirmation dialog a scanned
 * `gaindrive://pair` link opens, and the view model that does the sending.
 */

sealed interface PairSendResult {
	class Sent(val count: Int) : PairSendResult
	class Failure(val message: String) : PairSendResult
}

/**
 * In a view model for [TrackLinkViewModel]'s reason: the POST must survive
 * the recompositions a rotation brings, where an effect would be cancelled
 * mid-call with the link already marked handled.
 */
@HiltViewModel
class PairSendViewModel @Inject constructor(
	private val registry: ServerRegistry,
	private val wifi: WifiNetworks,
	private val json: Json,
) : ViewModel() {

	/**
	 * What a send would carry: enabled servers whose password is actually
	 * held. A blank one means "needs re-entering" and would import broken,
	 * so it is not offered at all.
	 */
	val sendable: StateFlow<List<ServerConfig>> = registry.enabledServers
		.map { list -> list.filter { it.password.isNotBlank() } }
		.stateIn(viewModelScope, SharingStarted.WhileSubscribed(5_000), emptyList())

	private val _sending = MutableStateFlow(false)
	val sending: StateFlow<Boolean> = _sending.asStateFlow()

	private val _result = MutableStateFlow<PairSendResult?>(null)
	val result: StateFlow<PairSendResult?> = _result.asStateFlow()

	fun send(link: PairLink) {
		if (_sending.value) return
		_sending.value = true
		viewModelScope.launch(Dispatchers.IO) {
			_result.value = doSend(link)
			_sending.value = false
		}
	}

	fun consumeResult() {
		_result.value = null
	}

	private suspend fun doSend(link: PairLink): PairSendResult {
		val servers = registry.enabledServers.first().filter { it.password.isNotBlank() }
		if (servers.isEmpty()) {
			return PairSendResult.Failure("no servers with a stored password")
		}
		val payload = PairPayload(
			servers.map {
				PairServer(
					name = it.name,
					url = it.url,
					username = it.username,
					password = it.password,
					browseByFolder = it.browseByFolder,
				)
			}
		)
		val text = json.encodeToString(PairPayload.serializer(), payload)
		val body = PairingCipher.encrypt(link.key, text)
		val request = Request.Builder()
			.url("http://${link.host}:${link.port}/${link.token}")
			.post(body.toRequestBody("text/plain".toMediaType()))
			.build()

		// Wi-Fi-bound first - the TV is on the LAN, which under a full-tunnel
		// VPN is not the default route - then unbound, because some VPNs make
		// the bound socket itself fail with EPERM (WiiMClient's discovery of
		// the same problem). A non-2xx answer breaks out instead: the TV was
		// reached and said no, and a different route would not change its mind.
		val factories = buildList<SocketFactory?> {
			wifi.network.value?.let { add(it.socketFactory) }
			add(null)
		}
		var lastError = "no route to the TV"
		for (factory in factories) {
			try {
				client(factory).newCall(request).execute().use { response ->
					if (response.isSuccessful) {
						Log.i(TAG, "pairing: sent ${servers.size} servers to ${link.host}")
						return PairSendResult.Sent(servers.size)
					}
					lastError = "the TV answered ${response.code}; " +
						"its pairing screen may have closed"
				}
				break
			} catch (failure: IOException) {
				val route = if (factory != null) "Wi-Fi-bound" else "default route"
				Log.i(TAG, "pairing: $route send failed - ${failure.message}")
				lastError = failure.message ?: "connection failed"
			}
		}
		return PairSendResult.Failure(lastError)
	}

	/**
	 * A bare client, never the injected shared one: that carries
	 * AuthInterceptor, which would append the account's Subsonic credentials
	 * to a request aimed at whatever device answered.
	 */
	private fun client(factory: SocketFactory?): OkHttpClient {
		val builder = OkHttpClient.Builder()
			.connectTimeout(SEND_TIMEOUT_MS, TimeUnit.MILLISECONDS)
			.readTimeout(SEND_TIMEOUT_MS, TimeUnit.MILLISECONDS)
		factory?.let { builder.socketFactory(it) }
		return builder.build()
	}

	private companion object {
		const val TAG = "GainDrivePair"
		const val SEND_TIMEOUT_MS = 5_000L
	}
}

/**
 * "Send your servers to the TV?" - what a scanned pairing QR opens. Plain
 * about what leaves the phone: server logins are the most sensitive thing
 * the app holds, and the QR proves someone stood in front of that TV, not
 * that the user meant to hand credentials over.
 */
@Composable
fun PairSendDialog(
	link: PairLink,
	onDone: () -> Unit,
	viewModel: PairSendViewModel = hiltViewModel(),
) {
	val context = LocalContext.current
	val granted = rememberLocalNetworkPermission()
	val servers by viewModel.sendable.collectAsStateWithLifecycle()
	val sending by viewModel.sending.collectAsStateWithLifecycle()
	val result by viewModel.result.collectAsStateWithLifecycle()

	LaunchedEffect(result) {
		val r = result ?: return@LaunchedEffect
		val message = when (r) {
			is PairSendResult.Sent ->
				if (r.count == 1) "Sent 1 server to the TV." else "Sent ${r.count} servers to the TV."
			is PairSendResult.Failure -> "Could not send: ${r.message}"
		}
		Toast.makeText(context, message, Toast.LENGTH_LONG).show()
		viewModel.consumeResult()
		// Success closes; a failure leaves the dialog up for another try
		// once the person has read why.
		if (r is PairSendResult.Sent) onDone()
	}

	AlertDialog(
		onDismissRequest = onDone,
		title = { Text("Send servers to the TV?") },
		text = {
			Column {
				Text(
					text = "This sends these server logins, passwords included, " +
						"to the device at ${link.host} on your network:",
					style = MaterialTheme.typography.bodyMedium,
				)
				servers.forEach {
					Text(
						text = "• " + it.name.ifBlank { it.url },
						style = MaterialTheme.typography.bodyMedium,
						modifier = Modifier.padding(top = 4.dp),
					)
				}
				if (servers.isEmpty()) {
					Text(
						text = "No servers with a stored password to send.",
						style = MaterialTheme.typography.bodyMedium,
						color = MaterialTheme.colorScheme.onSurfaceVariant,
						modifier = Modifier.padding(top = 4.dp),
					)
				}
				if (granted == false) {
					Text(
						text = "Sending needs local network access, which is " +
							"denied. Allow it for Gaindrive in the system app " +
							"settings.",
						style = MaterialTheme.typography.bodySmall,
						color = MaterialTheme.colorScheme.error,
						modifier = Modifier.padding(top = 8.dp),
					)
				}
			}
		},
		confirmButton = {
			TextButton(
				onClick = { viewModel.send(link) },
				enabled = !sending && servers.isNotEmpty() && granted == true,
			) {
				if (sending) {
					CircularProgressIndicator(
						modifier = Modifier.size(16.dp),
						strokeWidth = 2.dp,
					)
				} else {
					Text("Send")
				}
			}
		},
		dismissButton = {
			TextButton(onClick = onDone) { Text("Cancel") }
		},
	)
}
