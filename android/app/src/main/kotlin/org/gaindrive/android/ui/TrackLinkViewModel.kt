package org.gaindrive.android.ui

import androidx.lifecycle.ViewModel
import androidx.lifecycle.viewModelScope
import dagger.hilt.android.lifecycle.HiltViewModel
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.asStateFlow
import kotlinx.coroutines.launch
import org.gaindrive.android.data.TrackLink
import org.gaindrive.android.data.TrackLinkResolver
import org.gaindrive.android.data.TrackLinkResult
import javax.inject.Inject

/**
 * Resolving a track link involves a network call, so it runs here rather than
 * in the shell's own LaunchedEffect: this scope is activity-wide and survives
 * the recompositions a rotation brings, where an effect resolving the link
 * itself would be cancelled mid-call with the link already marked handled -
 * silently lost. The result is a one-shot consumed the way the player's
 * message is.
 */
@HiltViewModel
class TrackLinkViewModel @Inject constructor(
	private val resolver: TrackLinkResolver,
) : ViewModel() {

	private val _result = MutableStateFlow<TrackLinkResult?>(null)
	val result: StateFlow<TrackLinkResult?> = _result.asStateFlow()

	fun open(link: TrackLink) {
		viewModelScope.launch { _result.value = resolver.resolve(link) }
	}

	fun consumeResult() {
		_result.value = null
	}
}
