package org.gaindrive.android.ui.browse

import androidx.lifecycle.ViewModel
import androidx.lifecycle.viewModelScope
import dagger.hilt.android.lifecycle.HiltViewModel
import kotlinx.coroutines.ExperimentalCoroutinesApi
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.SharingStarted
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.combine
import kotlinx.coroutines.flow.flatMapLatest
import kotlinx.coroutines.flow.flow
import kotlinx.coroutines.flow.stateIn
import kotlinx.coroutines.launch
import org.gaindrive.android.data.LibraryRepository
import org.gaindrive.android.data.ServerSelection
import org.gaindrive.android.data.model.ArtistIndex
import org.gaindrive.android.data.model.ServerConfig
import org.gaindrive.android.data.model.ServerId
import org.gaindrive.android.net.runCatchingCancellable
import org.gaindrive.android.net.userMessage
import org.gaindrive.android.ui.Load
import javax.inject.Inject

@OptIn(ExperimentalCoroutinesApi::class)
@HiltViewModel
class ArtistsViewModel @Inject constructor(
	private val library: LibraryRepository,
	private val selection: ServerSelection,
) : ViewModel() {

	/** Bumped by [load] so a retry re-runs even when the server has not changed. */
	private val refresh = MutableStateFlow(0)

	/**
	 * Driven by the selected server rather than read once, so switching server
	 * reloads the list instead of leaving the previous library on screen.
	 */
	val state: StateFlow<Load<List<ArtistIndex>>> =
		combine(selection.current, refresh) { server, _ -> server }
			.flatMapLatest { server ->
				flow {
					emit(Load.Loading)
					emit(
						if (server == null) {
							Load.Failed("No server configured. Add one in Settings.")
						} else {
							runCatchingCancellable { library.artistIndexes(server.id) }
								.fold(
									onSuccess = { Load.Ready(it) },
									onFailure = { Load.Failed(it.userMessage()) },
								)
						}
					)
				}
			}
			.stateIn(viewModelScope, SharingStarted.WhileSubscribed(5_000), Load.Loading)

	val servers: StateFlow<List<ServerConfig>> = selection.available
		.stateIn(viewModelScope, SharingStarted.WhileSubscribed(5_000), emptyList())

	val currentServer: StateFlow<ServerConfig?> = selection.current
		.stateIn(viewModelScope, SharingStarted.WhileSubscribed(5_000), null)

	fun load() {
		refresh.value += 1
	}

	fun selectServer(id: ServerId) = viewModelScope.launch { selection.select(id) }
}
