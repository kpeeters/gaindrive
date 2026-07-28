package org.gaindrive.android.ui.browse

import androidx.lifecycle.ViewModel
import androidx.lifecycle.viewModelScope
import dagger.hilt.android.lifecycle.HiltViewModel
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.asStateFlow
import kotlinx.coroutines.launch
import org.gaindrive.android.data.LibraryRepository
import org.gaindrive.android.data.model.ArtistIndex
import org.gaindrive.android.net.runCatchingCancellable
import org.gaindrive.android.net.userMessage
import org.gaindrive.android.ui.Load
import javax.inject.Inject

@HiltViewModel
class ArtistsViewModel @Inject constructor(
	private val library: LibraryRepository,
) : ViewModel() {

	private val _state = MutableStateFlow<Load<List<ArtistIndex>>>(Load.Loading)
	val state: StateFlow<Load<List<ArtistIndex>>> = _state.asStateFlow()

	init {
		load()
	}

	fun load() {
		_state.value = Load.Loading
		viewModelScope.launch {
			// Sub-phase 2c replaces this with the browse scope. Until then the
			// first enabled server is the library.
			val server = library.enabledServers().firstOrNull()
			if (server == null) {
				_state.value = Load.Failed("No server configured. Add one in Settings.")
				return@launch
			}
			_state.value = runCatchingCancellable { library.artistIndexes(server.id) }
				.fold(
					onSuccess = { Load.Ready(it) },
					onFailure = { Load.Failed(it.userMessage()) },
				)
		}
	}
}
