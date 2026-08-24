package org.gaindrive.android.data

import kotlinx.coroutines.FlowPreview
import kotlinx.coroutines.flow.Flow
import kotlinx.coroutines.flow.combine
import kotlinx.coroutines.flow.debounce
import kotlinx.coroutines.flow.drop
import kotlinx.coroutines.flow.first
import kotlinx.coroutines.flow.map
import kotlinx.coroutines.flow.onStart
import org.gaindrive.android.data.model.BrowseScope
import org.gaindrive.android.data.model.BrowseState
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
	// A val because [settledOnline] reads it from a lambda, not just at
	// construction the way `registry` is used.
	private val connectivity: Connectivity,
	libraryRevision: LibraryRevision,
) {

	/** Every server that could be browsed, in registry order. */
	val available: Flow<List<ServerConfig>> = registry.enabledServers

	val scope: Flow<BrowseScope> =
		combine(registry.enabledServers, settings.browseScope) { servers, stored ->
			val chosen = servers.firstOrNull { it.id.value == stored }
			if (chosen != null) BrowseScope.OneServer(chosen.id) else BrowseScope.AllServers
		}

	/**
	 * Connectivity as the browse screens should see it: the current value
	 * straight away, and later changes only once they have settled.
	 *
	 * [drop] then [onStart] rather than debouncing the whole flow, because a
	 * `StateFlow` replays its current value and debouncing that too would hold
	 * up the first load of every screen by the settle window. What is worth
	 * waiting out is a Wi-Fi to cellular handover, which arrives as two changes
	 * in quick succession and would otherwise blank each list twice.
	 */
	@OptIn(FlowPreview::class)
	private val settledOnline: Flow<Boolean> = connectivity.online
		.drop(1)
		.debounce(CONNECTIVITY_SETTLE_MS)
		.onStart { emit(connectivity.online.value) }

	/**
	 * Scope, connectivity, the registry's revision and the library's together,
	 * because each changes what a browse screen should be showing and all four
	 * therefore have to trigger the same reload. Screens watch this rather than
	 * [scope] so none can be honoured while the others are quietly ignored.
	 *
	 * [LibraryRevision] is the app's own writes — an album promoted out of the
	 * uploads area is missing from one listing and new in another, and a screen
	 * that holds its list until its scope changes would show neither correctly.
	 *
	 * Deliberately sourced from [Connectivity.online] — losing the network and
	 * choosing offline mode are the same thing to a browse screen — and not
	 * from the offline setting directly. That is what makes this the *same*
	 * value `LibraryRepository` reads when it decides whether to trim a listing
	 * to what is stored. Two flows computed separately from the same setting
	 * would have no ordering between them, and a screen that reloaded first
	 * would rebuild its list from a repository that still believed it was
	 * offline, with nothing left to emit and correct it.
	 */
	val browse: Flow<BrowseState> =
		combine(
			scope,
			settledOnline,
			registry.revision,
			libraryRevision.revision,
		) { current, online, servers, library ->
			// Summed into the one field the screens already compare. Both
			// counters only ever increase, so the sum strictly increases
			// whenever either does — which is all `distinctUntilChanged`
			// needs. That two different pairs could add to the same number is
			// harmless: they cannot occur in that order.
			BrowseState(current, offline = !online, revision = servers + library)
		}

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

	/**
	 * The same state Settings shows, so the two controls cannot disagree.
	 *
	 * The *switch*, not the effective state — this backs a toggle, and a toggle
	 * that moved on its own because the signal dropped would be lying about
	 * what the user chose. [browse] is the one that folds in the network.
	 */
	val offline: Flow<Boolean> = settings.offlineMode

	suspend fun setOffline(enabled: Boolean) = settings.setOfflineMode(enabled)

	private companion object {
		/** Long enough to ride out a handover, short enough not to feel laggy. */
		const val CONNECTIVITY_SETTLE_MS = 500L
	}
}
