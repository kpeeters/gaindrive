package org.gaindrive.android.data

import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.flow.SharingStarted
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.combine
import kotlinx.coroutines.flow.stateIn
import javax.inject.Inject
import javax.inject.Singleton

/**
 * Whether the app should be talking to servers at all.
 *
 * Two reasons it might not be — there is no network, or the user asked for
 * offline mode — and almost nothing cares which. Everything therefore reads
 * [online] rather than [NetworkMonitor] directly, so the manual switch is not
 * something each call site has to remember to honour.
 *
 * [offlineByChoice] exists only for the places that must tell the two apart,
 * which is the banner: "no signal" and "you turned this on" want different
 * words, and only one of them is a problem.
 */
@Singleton
class Connectivity @Inject constructor(
	monitor: NetworkMonitor,
	settings: SettingsStore,
	scope: CoroutineScope,
) {

	val online: StateFlow<Boolean> =
		combine(monitor.online, settings.offlineMode) { hasNetwork, offline ->
			hasNetwork && !offline
		}.stateIn(
			scope = scope,
			// Eagerly, not WhileSubscribed: the repository reads `.value` on
			// every query rather than collecting, so this has to be current
			// whether or not any screen happens to be watching.
			started = SharingStarted.Eagerly,
			initialValue = true,
		)

	val offlineByChoice: StateFlow<Boolean> =
		settings.offlineMode.stateIn(scope, SharingStarted.Eagerly, false)
}
