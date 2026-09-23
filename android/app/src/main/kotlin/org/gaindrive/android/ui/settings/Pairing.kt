package org.gaindrive.android.ui.settings

import android.graphics.Bitmap
import androidx.compose.foundation.Image
import androidx.compose.foundation.background
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.layout.width
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.DisposableEffect
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.remember
import androidx.compose.ui.Modifier
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.graphics.FilterQuality
import androidx.compose.ui.graphics.ImageBitmap
import androidx.compose.ui.graphics.asImageBitmap
import androidx.compose.ui.unit.dp
import androidx.hilt.lifecycle.viewmodel.compose.hiltViewModel
import androidx.lifecycle.ViewModel
import androidx.lifecycle.viewModelScope
import androidx.lifecycle.compose.collectAsStateWithLifecycle
import com.google.zxing.BarcodeFormat
import com.google.zxing.qrcode.QRCodeWriter
import dagger.hilt.android.lifecycle.HiltViewModel
import javax.inject.Inject
import kotlinx.coroutines.delay
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.asStateFlow
import kotlinx.coroutines.flow.first
import kotlinx.coroutines.launch
import org.gaindrive.android.data.PairServer
import org.gaindrive.android.data.ServerRegistry
import org.gaindrive.android.data.model.ServerConfig
import org.gaindrive.android.net.PairingServer
import org.gaindrive.android.ui.rememberLocalNetworkPermission

/**
 * The TV side of QR pairing: the pane beside the add-server form, the view
 * model that runs the listener, and the QR itself.
 */

sealed interface PairingUiState {
	data object Idle : PairingUiState

	/** No LAN address to listen on. */
	data object Unavailable : PairingUiState

	class Waiting(val uri: String) : PairingUiState

	/**
	 * [received] arrived, [added] were new. The difference is servers this
	 * TV already had, matched on url + username.
	 */
	class Imported(val added: Int, val received: Int) : PairingUiState
}

@HiltViewModel
class PairingViewModel @Inject constructor(
	private val pairing: PairingServer,
	private val registry: ServerRegistry,
) : ViewModel() {

	private val _state = MutableStateFlow<PairingUiState>(PairingUiState.Idle)
	val state: StateFlow<PairingUiState> = _state.asStateFlow()

	fun start() {
		val uri = pairing.start(::received)
		_state.value =
			if (uri != null) PairingUiState.Waiting(uri) else PairingUiState.Unavailable
	}

	fun stop() = pairing.stop()

	private fun received(servers: List<PairServer>) {
		// Arrives on the listener's IO dispatcher; the registry writes and the
		// state change belong to this scope.
		viewModelScope.launch {
			val existing = registry.servers.first()
			var added = 0
			for (server in servers) {
				val url = ServerConfig.normaliseUrl(server.url)
				val user = ServerConfig.normaliseUsername(server.username)
				if (url.isEmpty() || user.isEmpty() || server.password.isEmpty()) continue
				// The registry itself keeps no uniqueness, so a rescan of the
				// same QR would double every server without this.
				val duplicate = existing.any {
					it.url.equals(url, ignoreCase = true) && it.username == user
				}
				if (duplicate) continue
				registry.add(server.name, server.url, server.username,
					server.password, server.browseByFolder)
				added++
			}
			_state.value = PairingUiState.Imported(added = added, received = servers.size)
		}
	}
}

/**
 * "Or scan from your phone", shown on a TV beside the add-server form while
 * nothing is configured by hand. The listener lives exactly as long as this
 * is composed. [onImported] fires shortly after servers arrive, once the
 * summary line has had a moment to be read.
 */
@Composable
fun PairingPane(
	onImported: () -> Unit,
	modifier: Modifier = Modifier,
	viewModel: PairingViewModel = hiltViewModel(),
) {
	val granted = rememberLocalNetworkPermission()
	val state by viewModel.state.collectAsStateWithLifecycle()

	if (granted == true) {
		DisposableEffect(Unit) {
			viewModel.start()
			onDispose { viewModel.stop() }
		}
	}

	val imported = state as? PairingUiState.Imported
	LaunchedEffect(imported) {
		if (imported != null) {
			delay(IMPORTED_LINGER_MS)
			onImported()
		}
	}

	Column(modifier = modifier.width(PANE_WIDTH)) {
		Text(
			text = "Or scan from your phone",
			style = MaterialTheme.typography.titleSmall,
			modifier = Modifier.padding(bottom = 8.dp),
		)
		when {
			granted == false -> Text(
				text = "Receiving from a phone needs local network access, " +
					"which is denied. Allow it for Gaindrive in the system " +
					"app settings, or fill in the form instead.",
				style = MaterialTheme.typography.bodySmall,
				color = MaterialTheme.colorScheme.onSurfaceVariant,
			)

			imported != null -> Text(
				text = when {
					imported.added > 0 -> "Added ${imported.added} " +
						if (imported.added == 1) "server." else "servers."
					imported.received > 0 -> "Those servers are already here."
					else -> "The phone had no servers to send."
				},
				style = MaterialTheme.typography.bodyMedium,
				color = MaterialTheme.colorScheme.primary,
			)

			state is PairingUiState.Unavailable -> Text(
				text = "No network connection to listen on.",
				style = MaterialTheme.typography.bodySmall,
				color = MaterialTheme.colorScheme.onSurfaceVariant,
			)

			else -> {
				val waiting = state as? PairingUiState.Waiting
				if (waiting != null) {
					// White behind the code whatever the theme: a QR is read
					// by a camera, not by the palette.
					Image(
						bitmap = remember(waiting.uri) { qrBitmap(waiting.uri) },
						contentDescription = "Pairing QR code",
						// None, not bilinear: scaling a QR softly smears the
						// cells, and a camera wants edges.
						filterQuality = FilterQuality.None,
						modifier = Modifier
							.size(QR_SIZE)
							.background(Color.White)
							.padding(8.dp),
					)
					Text(
						text = "Point your phone's camera here. The gaindrive " +
							"app on it will offer to send its servers over.",
						style = MaterialTheme.typography.bodySmall,
						color = MaterialTheme.colorScheme.onSurfaceVariant,
						modifier = Modifier.padding(top = 8.dp),
					)
				}
				// Idle: the permission dialog is still up; nothing to say yet.
			}
		}
	}
}

/**
 * Rendered at module resolution - zxing takes a 0x0 request as "natural
 * size", one pixel per module plus the quiet zone - and scaled up by the
 * Image, which keeps the cells crisp.
 */
private fun qrBitmap(content: String): ImageBitmap {
	val matrix = QRCodeWriter().encode(content, BarcodeFormat.QR_CODE, 0, 0)
	val width = matrix.width
	val height = matrix.height
	val pixels = IntArray(width * height) { i ->
		if (matrix.get(i % width, i / width)) BLACK else WHITE
	}
	return Bitmap.createBitmap(pixels, width, height, Bitmap.Config.ARGB_8888)
		.asImageBitmap()
}

private const val BLACK = 0xFF000000.toInt()
private const val WHITE = 0xFFFFFFFF.toInt()

private val PANE_WIDTH = 224.dp
private val QR_SIZE = 176.dp

/** Long enough to read "Added 2 servers." before the screen closes itself. */
private const val IMPORTED_LINGER_MS = 1_500L
