package org.gaindrive.android.ui.settings

import androidx.compose.foundation.clickable
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.lazy.items
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.filled.Add
import androidx.compose.material.icons.filled.MoreVert
import androidx.compose.material3.Card
import androidx.compose.material3.DropdownMenu
import androidx.compose.material3.DropdownMenuItem
import androidx.compose.material3.FloatingActionButton
import androidx.compose.material3.Icon
import androidx.compose.material3.IconButton
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.dp
import androidx.hilt.navigation.compose.hiltViewModel
import androidx.lifecycle.compose.collectAsStateWithLifecycle
import org.gaindrive.android.data.DEFAULT_CAST_PORT
import org.gaindrive.android.data.ManualCastDevice

/** What a Chromecast is sent, which is not always what this phone would play. */
@Composable
fun CastSettingsScreen(
	onBack: (() -> Unit)?,
	viewModel: SettingsViewModel = hiltViewModel(),
	devicesViewModel: CastDevicesViewModel = hiltViewModel(),
) {
	val state by viewModel.state.collectAsStateWithLifecycle()
	val devices by devicesViewModel.devices.collectAsStateWithLifecycle()
	val draft by devicesViewModel.draft.collectAsStateWithLifecycle()

	SettingsScaffold(
		title = "Casting",
		onBack = onBack,
		floatingActionButton = {
			FloatingActionButton(onClick = devicesViewModel::add) {
				Icon(Icons.Default.Add, contentDescription = "Add device")
			}
		},
	) {
		item {
			SwitchRow(
				title = "Cast at original quality",
				// Says which casts it applies to, because that is the whole
				// shape of the setting and there is no way to see it otherwise.
				subtitle = "Send the file as it is stored when the player fetches it " +
					"from the server itself. Otherwise casting uses the same " +
					"quality as this phone.",
				checked = state.castOriginal,
				onCheckedChange = viewModel::setCastOriginal,
			)
		}

		item {
			Text(
				// The three ways it will silently not apply. Each is a question
				// the track info dialog can answer after the fact, and this is
				// the only place that answers it beforehand.
				text = "A cast relayed through this phone keeps the streaming " +
					"quality: those bytes cross this phone's connection, which is " +
					"the situation relaying exists for. Your account's bitrate " +
					"limit still applies. And a file in a format the player cannot " +
					"decode is converted anyway, rather than failing to play.\n\n" +
					"The info button in Now Playing shows what is actually being " +
					"sent, and how.",
				style = MaterialTheme.typography.bodySmall,
				color = MaterialTheme.colorScheme.onSurfaceVariant,
			)
		}

		item { SectionTitle("Devices") }

		item {
			Text(
				// Why the section exists at all. Discovery is a mechanism that
				// can fail for reasons no client can fix — a device asleep
				// enough not to wake for multicast, an access point that drops
				// it, or a responder that has stopped answering even a direct
				// unicast query. Naming the address sidesteps all of it.
				text = "Devices found on the network appear in the cast picker by " +
					"themselves. Add one here only when it does not: a television " +
					"that answers on its address but has stopped announcing itself " +
					"cannot be discovered by any app, including this one.",
				style = MaterialTheme.typography.bodySmall,
				color = MaterialTheme.colorScheme.onSurfaceVariant,
			)
		}

		if (devices.isEmpty()) {
			item {
				Text(
					text = "No devices added.",
					style = MaterialTheme.typography.bodyMedium,
					color = MaterialTheme.colorScheme.onSurfaceVariant,
				)
			}
		}

		items(devices, key = { it.id }) { device ->
			ManualDeviceRow(
				device = device,
				onEdit = { devicesViewModel.edit(device) },
				onRemove = { devicesViewModel.remove(device.id) },
			)
		}
	}

	draft?.let {
		CastDeviceDialog(
			draft = it,
			onAddress = devicesViewModel::onAddress,
			onName = devicesViewModel::onName,
			onPort = devicesViewModel::onPort,
			onTest = devicesViewModel::test,
			onSave = devicesViewModel::save,
			onDismiss = devicesViewModel::dismiss,
		)
	}
}

/**
 * One added device. Shaped like `ServerRow`, minus the reordering — the order
 * of cast devices means nothing, so there is nothing to move.
 */
@Composable
private fun ManualDeviceRow(
	device: ManualCastDevice,
	onEdit: () -> Unit,
	onRemove: () -> Unit,
) {
	var menuOpen by remember { mutableStateOf(false) }

	Card(modifier = Modifier.fillMaxWidth()) {
		Row(
			modifier = Modifier
				.fillMaxWidth()
				.clickable(onClick = onEdit)
				.padding(16.dp),
			verticalAlignment = Alignment.CenterVertically,
			horizontalArrangement = Arrangement.spacedBy(12.dp),
		) {
			Column(modifier = Modifier.weight(1f)) {
				Text(
					// A device with no name typed is shown by its address, the
					// same fallback the picker makes.
					text = device.name.ifBlank { device.address },
					style = MaterialTheme.typography.titleSmall,
					maxLines = 1,
					overflow = TextOverflow.Ellipsis,
				)
				Text(
					// The port is shown only when it is not the usual one, so an
					// ordinary row reads as a plain address.
					text = if (device.port == DEFAULT_CAST_PORT) {
						device.address
					} else {
						"${device.address}:${device.port}"
					},
					style = MaterialTheme.typography.bodySmall,
					color = MaterialTheme.colorScheme.onSurfaceVariant,
					maxLines = 1,
					overflow = TextOverflow.Ellipsis,
				)
			}

			Box {
				IconButton(onClick = { menuOpen = true }) {
					Icon(Icons.Default.MoreVert, contentDescription = "More")
				}
				DropdownMenu(expanded = menuOpen, onDismissRequest = { menuOpen = false }) {
					DropdownMenuItem(
						text = { Text("Edit") },
						onClick = {
							menuOpen = false
							onEdit()
						},
					)
					DropdownMenuItem(
						text = { Text("Remove") },
						onClick = {
							menuOpen = false
							onRemove()
						},
					)
				}
			}
		}
	}
}
