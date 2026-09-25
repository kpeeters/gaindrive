package org.gaindrive.android.ui

import androidx.compose.runtime.compositionLocalOf
import androidx.lifecycle.ViewModel
import androidx.lifecycle.viewModelScope
import dagger.hilt.android.lifecycle.HiltViewModel
import kotlinx.coroutines.flow.SharedFlow
import kotlinx.coroutines.flow.SharingStarted
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.combine
import kotlinx.coroutines.flow.stateIn
import org.gaindrive.android.data.StarStore
import org.gaindrive.android.data.model.ItemRef
import org.gaindrive.android.data.model.StarKind
import javax.inject.Inject

/**
 * The star state every row, sheet and header reads, over the `starredAt` its
 * own model was loaded with. See [StarStore] for why a loaded model cannot be
 * trusted after a toggle.
 */
class Stars(
	private val overrides: Map<ItemRef, Boolean> = emptyMap(),
	private val busy: Set<ItemRef> = emptySet(),
	private val store: StarStore? = null,
) {
	fun isStarred(ref: ItemRef, fallback: Boolean): Boolean = overrides[ref] ?: fallback

	fun isBusy(ref: ItemRef): Boolean = ref in busy

	fun toggle(ref: ItemRef, kind: StarKind, currently: Boolean) {
		store?.toggle(ref, kind, currently)
	}
}

/** Ambient for the same reason as [LocalAvailability]: every track row wants it. */
val LocalStars = compositionLocalOf { Stars() }

@HiltViewModel
class StarsViewModel @Inject constructor(
	store: StarStore,
) : ViewModel() {

	val state: StateFlow<Stars> =
		combine(store.overrides, store.busy) { overrides, busy ->
			Stars(overrides, busy, store)
		}.stateIn(
			scope = viewModelScope,
			started = SharingStarted.WhileSubscribed(5_000),
			initialValue = Stars(store = store),
		)

	/** Refusals, for the shell to toast: the sheet that asked has closed by then. */
	val errors: SharedFlow<String> = store.errors
}
