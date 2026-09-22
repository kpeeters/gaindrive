package org.gaindrive.android.ui.player

import androidx.compose.foundation.clickable
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.filled.Cast
import androidx.compose.material.icons.filled.CastConnected
import androidx.compose.material3.CircularProgressIndicator
import androidx.compose.material3.ExperimentalMaterial3Api
import androidx.compose.material3.Icon
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.ModalBottomSheet
import androidx.compose.material3.Text
import androidx.compose.material3.TextButton
import androidx.compose.material3.rememberModalBottomSheetState
import androidx.compose.runtime.Composable
import androidx.compose.runtime.DisposableEffect
import androidx.compose.runtime.getValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.dp
import androidx.hilt.lifecycle.viewmodel.compose.hiltViewModel
import androidx.lifecycle.compose.collectAsStateWithLifecycle
import org.gaindrive.android.playback.cast.CastDevice
import org.gaindrive.android.ui.rememberLocalNetworkPermission

/**
 * Picks a Chromecast.
 *
 * A plain sheet rather than the system route picker: `MediaRouteProvider` is
 * what `CAST.md` asks for and is still the destination, but it is a surface of
 * its own and the protocol underneath needs to be exercised first. Nothing here
 * is in the protocol's way when it lands.
 */
@OptIn(ExperimentalMaterial3Api::class)
@Composable
fun CastDeviceSheet(
	onDismiss: () -> Unit,
	viewModel: CastViewModel = hiltViewModel(),
) {
	val devices by viewModel.devices.collectAsStateWithLifecycle()
	val manual by viewModel.manualDevices.collectAsStateWithLifecycle()
	val connected by viewModel.connected.collectAsStateWithLifecycle()
	val sheetState = rememberModalBottomSheetState(skipPartiallyExpanded = true)
	val networkGranted = rememberLocalNetworkPermission()

	// Scanning costs a live multicast conversation, so it lasts exactly as long
	// as this sheet does. It waits for Android 17's local-network grant: started
	// without it, the scan either fails silently or drags a system consent
	// dialog into every open of this sheet.
	DisposableEffect(networkGranted) {
		if (networkGranted == true) viewModel.startDiscovery()
		onDispose {
			if (networkGranted == true) viewModel.stopDiscovery()
		}
	}

	ModalBottomSheet(onDismissRequest = onDismiss, sheetState = sheetState) {
		Column(modifier = Modifier.padding(bottom = 24.dp)) {
			Text(
				text = "Cast to",
				style = MaterialTheme.typography.titleMedium,
				modifier = Modifier.padding(start = 24.dp, end = 24.dp, bottom = 8.dp),
			)

			// Denial kills more than the scan: the Cast connection itself is a
			// LAN socket, so even a manually added device is out of reach. Said
			// here in full, because the sheet is otherwise an eternal spinner.
			if (networkGranted == false) {
				Text(
					text = "Casting needs access to devices on your local " +
						"network, which is denied. Allow it for Gaindrive in " +
						"the system app settings.",
					style = MaterialTheme.typography.bodyMedium,
					color = MaterialTheme.colorScheme.onSurfaceVariant,
					modifier = Modifier.padding(horizontal = 24.dp, vertical = 16.dp),
				)
			}

			// The spinner is about discovery alone, so it keeps running while
			// manual devices are listed below it — a device that was added by
			// hand is no reason to stop looking for the others.
			if (devices.isEmpty() && networkGranted != false) {
				Row(
					modifier = Modifier
						.fillMaxWidth()
						.padding(horizontal = 24.dp, vertical = 16.dp),
					verticalAlignment = Alignment.CenterVertically,
					horizontalArrangement = Arrangement.spacedBy(16.dp),
				) {
					CircularProgressIndicator(modifier = Modifier.size(20.dp))
					Text(
						text = "Looking for devices…",
						style = MaterialTheme.typography.bodyMedium,
						color = MaterialTheme.colorScheme.onSurfaceVariant,
					)
				}
			}

			devices.forEach { device ->
				DeviceRow(
					device = device,
					connected = device.id == connected?.id,
					onClick = {
						viewModel.select(device)
						onDismiss()
					},
				)
			}

			if (manual.isNotEmpty()) {
				// Its own heading rather than one merged list: when discovery is
				// the thing that has failed, which devices arrived by which route
				// is the most useful fact on this sheet.
				Text(
					text = "Added manually",
					style = MaterialTheme.typography.titleSmall,
					color = MaterialTheme.colorScheme.onSurfaceVariant,
					modifier = Modifier.padding(
						start = 24.dp,
						end = 24.dp,
						top = 16.dp,
						bottom = 4.dp,
					),
				)
				manual.forEach { device ->
					DeviceRow(
						device = device,
						connected = device.id == connected?.id,
						onClick = {
							viewModel.select(device)
							onDismiss()
						},
					)
				}
			} else if (devices.isEmpty() && networkGranted != false) {
				// Only worth saying when the sheet is otherwise empty: this is
				// exactly the moment someone needs to know the option exists.
				// Not said over a denial, where adding an address fixes nothing.
				Text(
					text = "A device that has stopped announcing itself can be added " +
						"by address in Settings → Casting.",
					style = MaterialTheme.typography.bodySmall,
					color = MaterialTheme.colorScheme.onSurfaceVariant,
					modifier = Modifier.padding(horizontal = 24.dp, vertical = 8.dp),
				)
			}

			if (connected != null) {
				TextButton(
					onClick = {
						viewModel.disconnect()
						onDismiss()
					},
					modifier = Modifier.padding(start = 16.dp, top = 8.dp),
				) {
					Text("Stop casting")
				}
			}
		}
	}
}

@Composable
private fun DeviceRow(device: CastDevice, connected: Boolean, onClick: () -> Unit) {
	Row(
		modifier = Modifier
			.fillMaxWidth()
			.clickable(onClick = onClick)
			.padding(horizontal = 24.dp, vertical = 12.dp),
		verticalAlignment = Alignment.CenterVertically,
		horizontalArrangement = Arrangement.spacedBy(16.dp),
	) {
		Icon(
			imageVector = if (connected) Icons.Default.CastConnected else Icons.Default.Cast,
			contentDescription = null,
			tint = if (connected) {
				MaterialTheme.colorScheme.primary
			} else {
				MaterialTheme.colorScheme.onSurfaceVariant
			},
		)
		Column(modifier = Modifier.weight(1f)) {
			Text(
				text = device.name,
				style = MaterialTheme.typography.bodyLarge,
				color = if (connected) {
					MaterialTheme.colorScheme.primary
				} else {
					MaterialTheme.colorScheme.onSurface
				},
				maxLines = 1,
				overflow = TextOverflow.Ellipsis,
			)
			// The address is the one thing that distinguishes two devices the
			// user gave the same name, and it is what a connection failure will
			// be about. The announced model precedes it where there is one —
			// a manually added device has no announcement, so it shows the
			// address alone as it always has.
			Text(
				text = listOfNotNull(device.model, device.address).joinToString(" · "),
				style = MaterialTheme.typography.bodySmall,
				color = MaterialTheme.colorScheme.onSurfaceVariant,
			)
		}
	}
}
