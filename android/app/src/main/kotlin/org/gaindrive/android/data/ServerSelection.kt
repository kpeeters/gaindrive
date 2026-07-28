package org.gaindrive.android.data

import kotlinx.coroutines.flow.Flow
import kotlinx.coroutines.flow.combine
import org.gaindrive.android.data.model.ServerConfig
import org.gaindrive.android.data.model.ServerId
import javax.inject.Inject
import javax.inject.Singleton

/**
 * Which server the library screens are currently showing.
 *
 * Deliberately narrower than the browse scope sub-phase 2c will introduce: this
 * only ever names one server, with no merged "all servers" option. It exists so
 * the browse screens stop reaching for "the first enabled server" — a
 * placeholder that would otherwise be copied into every screen added from here
 * on, and then have to be unpicked from all of them.
 */
@Singleton
class ServerSelection @Inject constructor(
	private val settings: SettingsStore,
	registry: ServerRegistry,
) {

	/**
	 * The effective server: the stored choice when it is still enabled,
	 * otherwise the first enabled one, otherwise null.
	 *
	 * Falling back matters — the stored id survives the server being disabled
	 * or removed, and a dangling choice would leave the library permanently
	 * empty with no hint as to why.
	 */
	val current: Flow<ServerConfig?> =
		combine(registry.enabledServers, settings.selectedServerId) { servers, selectedId ->
			servers.firstOrNull { it.id.value == selectedId } ?: servers.firstOrNull()
		}

	/** Every server that could be selected, in registry order. */
	val available: Flow<List<ServerConfig>> = registry.enabledServers

	suspend fun select(id: ServerId) = settings.setSelectedServerId(id.value)
}
