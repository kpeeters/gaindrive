package org.gaindrive.android.ui

import androidx.lifecycle.ViewModel
import androidx.lifecycle.viewModelScope
import dagger.hilt.android.lifecycle.HiltViewModel
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.SharingStarted
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.asStateFlow
import kotlinx.coroutines.flow.map
import kotlinx.coroutines.flow.stateIn
import kotlinx.coroutines.launch
import org.gaindrive.android.data.cache.Pin
import org.gaindrive.android.data.cache.PinKind
import org.gaindrive.android.data.cache.PinRepository
import org.gaindrive.android.data.cache.PinResult
import org.gaindrive.android.data.model.ItemRef
import org.gaindrive.android.ui.components.formatBytes
import javax.inject.Inject

/**
 * Pinning, shared by everything that offers it — the album bar, the playlist
 * bar and the track sheet all mean the same thing by "download".
 */
@HiltViewModel
class PinViewModel @Inject constructor(
	private val pins: PinRepository,
) : ViewModel() {

	val pinnedRefs: StateFlow<Set<String>> = pins.pins
		.map { list -> list.map(Pin::ref).map(ItemRef::encode).toSet() }
		.stateIn(viewModelScope, SharingStarted.WhileSubscribed(5_000), emptySet())

	private val _message = MutableStateFlow<String?>(null)

	/** A refusal worth explaining; success says nothing and just starts. */
	val message: StateFlow<String?> = _message.asStateFlow()

	fun consumeMessage() {
		_message.value = null
	}

	fun toggle(ref: ItemRef, kind: PinKind) = viewModelScope.launch {
		if (pins.isPinned(ref)) {
			pins.unpin(ref)
			return@launch
		}
		when (val result = pins.pin(ref, kind)) {
			is PinResult.Ok -> Unit

			// The track list is what a pin expands to, so an album nobody has
			// opened yet has nothing to download.
			is PinResult.NotKnownYet ->
				_message.value = "Open this once while online, then download it."

			is PinResult.TooLarge -> _message.value =
				"Downloads would need ${formatBytes(result.neededBytes)}, and the " +
					"cache holds ${formatBytes(result.capBytes)}. Raise the maximum " +
					"size in Settings."
		}
	}
}
