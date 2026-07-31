package org.gaindrive.android.ui

import androidx.compose.runtime.compositionLocalOf
import androidx.lifecycle.ViewModel
import androidx.lifecycle.viewModelScope
import dagger.hilt.android.lifecycle.HiltViewModel
import kotlinx.coroutines.flow.SharingStarted
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.combine
import kotlinx.coroutines.flow.stateIn
import org.gaindrive.android.data.NetworkMonitor
import org.gaindrive.android.data.cache.AudioCache
import org.gaindrive.android.data.cache.PinRepository
import org.gaindrive.android.data.model.ItemRef
import javax.inject.Inject

/** Whether a track can be played right now, and from where. */
enum class Availability {
	/** Held locally in full; plays with no server involved. */
	STORED,

	/** Not stored, but there is a network to fetch it over. */
	STREAMABLE,

	/** Not stored and nothing to fetch it over. */
	UNAVAILABLE,
}

data class AvailabilityState(
	val online: Boolean = true,
	val storedKeys: Set<String> = emptySet(),
	val pinnedKeys: Set<String> = emptySet(),
) {
	fun of(ref: ItemRef): Availability {
		val key = ref.encode()
		return when {
			key in storedKeys -> Availability.STORED
			online -> Availability.STREAMABLE
			else -> Availability.UNAVAILABLE
		}
	}

	/** Kept on purpose, as opposed to merely happening to be cached. */
	fun isDownloaded(ref: ItemRef): Boolean {
		val key = ref.encode()
		return key in pinnedKeys && key in storedKeys
	}
}

/**
 * Ambient rather than threaded through every screen.
 *
 * Nearly every row wants this and no screen has an opinion about it, so passing
 * it down by hand would mean a parameter on every list, view model and route
 * for something none of them are about.
 */
val LocalAvailability = compositionLocalOf { AvailabilityState() }

@HiltViewModel
class AvailabilityViewModel @Inject constructor(
	audioCache: AudioCache,
	pins: PinRepository,
	network: NetworkMonitor,
) : ViewModel() {

	val state: StateFlow<AvailabilityState> =
		combine(
			network.online,
			audioCache.cachedKeys,
			pins.protectedKeys,
			::AvailabilityState,
		).stateIn(
			scope = viewModelScope,
			started = SharingStarted.WhileSubscribed(5_000),
			initialValue = AvailabilityState(),
		)
}
