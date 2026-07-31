package org.gaindrive.android.data

import kotlinx.coroutines.flow.Flow
import kotlinx.coroutines.flow.combine
import kotlinx.coroutines.flow.first
import kotlinx.coroutines.flow.map
import org.gaindrive.android.data.model.BrowseScope
import org.gaindrive.android.data.model.ServerConfig
import org.gaindrive.android.data.model.ServerId
import javax.inject.Inject
import javax.inject.Singleton

/**
 * What the library screens are currently showing: one named server, or all of
 * them merged.
 *
 * The choice is remembered, and a stored choice that no longer resolves — the
 * server was disabled or removed — falls back to all servers rather than
 * leaving the library permanently empty with no hint as to why.
 */
@Singleton
class ServerSelection @Inject constructor(
	private val settings: SettingsStore,
	registry: ServerRegistry,
) {

	/** Every server that could be browsed, in registry order. */
	val available: Flow<List<ServerConfig>> = registry.enabledServers

	val scope: Flow<BrowseScope> =
		combine(registry.enabledServers, settings.browseScope) { servers, stored ->
			val chosen = servers.firstOrNull { it.id.value == stored }
			if (chosen != null) BrowseScope.OneServer(chosen.id) else BrowseScope.AllServers
		}

	/**
	 * Scope and offline mode together, because both change what a browse screen
	 * should be showing and both therefore have to trigger the same reload.
	 * Screens watch this rather than [scope] so neither can be honoured while
	 * the other is quietly ignored.
	 */
	val browse: Flow<BrowseState> =
		combine(scope, settings.offlineMode) { current, offline -> BrowseState(current, offline) }

	/**
	 * The servers the current scope covers, in registry order — which is what
	 * every fan-out iterates and what breaks ties when rows merge.
	 */
	val scoped: Flow<List<ServerConfig>> =
		combine(registry.enabledServers, scope) { servers, current ->
			when (current) {
				is BrowseScope.AllServers -> servers
				is BrowseScope.OneServer -> servers.filter { it.id == current.id }
			}
		}

	/**
	 * Server names by id, for the row badges — empty unless several servers are
	 * genuinely in play, since a single-server library must look like one.
	 * Badges are therefore suppressed both in single-server scope and when only
	 * one server is configured, without any screen having to know that rule.
	 */
	val badgeNames: Flow<Map<ServerId, String>> = scoped.map { list ->
		if (list.size < 2) emptyMap() else list.associate { it.id to it.name }
	}

	/**
	 * Nothing to browse at all, which a fan-out cannot distinguish from a
	 * library that is simply empty — both come back with no rows and no
	 * failures.
	 */
	suspend fun hasNoServers(): Boolean = available.first().isEmpty()

	suspend fun select(id: ServerId) = settings.setBrowseScope(id.value)

	suspend fun selectAllServers() = settings.setBrowseScope(BrowseScope.ALL_STORED)

	/** The same state Settings shows, so the two controls cannot disagree. */
	val offline: Flow<Boolean> = settings.offlineMode

	suspend fun setOffline(enabled: Boolean) = settings.setOfflineMode(enabled)
}
