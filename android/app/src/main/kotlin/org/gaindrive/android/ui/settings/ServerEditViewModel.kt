package org.gaindrive.android.ui.settings

import androidx.lifecycle.SavedStateHandle
import androidx.lifecycle.ViewModel
import androidx.lifecycle.viewModelScope
import dagger.hilt.android.lifecycle.HiltViewModel
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.asStateFlow
import kotlinx.coroutines.flow.update
import androidx.navigation.toRoute
import kotlinx.coroutines.launch
import org.gaindrive.android.data.ConnectionTester
import org.gaindrive.android.ui.Route
import org.gaindrive.android.data.ServerRegistry
import org.gaindrive.android.data.model.ServerConfig
import org.gaindrive.android.data.model.ServerId
import org.gaindrive.android.net.ConnectionTest
import javax.inject.Inject

data class ServerEditUiState(
	val name: String = "",
	val url: String = "",
	val username: String = "",
	val password: String = "",
	val browseByFolder: Boolean = false,
	val isNew: Boolean = true,
	val testing: Boolean = false,
	val testResult: ConnectionTest? = null,
	val saved: Boolean = false,
) {
	val canSave: Boolean
		get() = url.isNotBlank() && username.isNotBlank() &&
			(password.isNotBlank() || !isNew)
}

@HiltViewModel
class ServerEditViewModel @Inject constructor(
	private val registry: ServerRegistry,
	private val tester: ConnectionTester,
	savedStateHandle: SavedStateHandle,
) : ViewModel() {

	// Read through the type-safe route rather than a string key, so renaming
	// the route's field is a compile error instead of a silent null.
	private val serverId: ServerId? =
		savedStateHandle.toRoute<Route.ServerEdit>().serverId?.let { ServerId(it) }

	private val _state = MutableStateFlow(ServerEditUiState(isNew = serverId == null))
	val state: StateFlow<ServerEditUiState> = _state.asStateFlow()

	init {
		serverId?.let { id ->
			viewModelScope.launch {
				val existing = registry.get(id) ?: return@launch
				_state.update {
					it.copy(
						name = existing.name,
						url = existing.url,
						username = existing.username,
						// Deliberately not prefilled: an existing password is
						// kept by leaving this blank, so it never has to make
						// a round trip through the UI just to rename a server.
						password = "",
						browseByFolder = existing.browseByFolder,
						isNew = false,
					)
				}
			}
		}
	}

	fun onName(v: String) = _state.update { it.copy(name = v, testResult = null) }
	fun onUrl(v: String) = _state.update { it.copy(url = v, testResult = null) }
	fun onUsername(v: String) = _state.update { it.copy(username = v, testResult = null) }
	fun onPassword(v: String) = _state.update { it.copy(password = v, testResult = null) }

	/**
	 * Alone among the setters in not clearing [ServerEditUiState.testResult]:
	 * the others change who or where we are connecting to, which invalidates a
	 * result already on screen. This changes only which half of the API will be
	 * asked once we are in, and re-testing costs a request either way.
	 */
	fun onBrowseByFolder(v: Boolean) = _state.update { it.copy(browseByFolder = v) }

	/**
	 * Testing an edit that leaves the password blank would test the wrong
	 * thing, so the caller is expected to disable the action in that case.
	 */
	fun test() {
		val s = _state.value
		_state.update { it.copy(testing = true, testResult = null) }
		viewModelScope.launch {
			// Normalised the same way saving does, or the test would pass on
			// credentials the app will never actually send.
			val result = tester.test(
				s.url,
				ServerConfig.normaliseUsername(s.username),
				ServerConfig.normalisePassword(s.password),
				s.browseByFolder,
			)
			_state.update { it.copy(testing = false, testResult = result) }
		}
	}

	fun save() {
		val s = _state.value
		viewModelScope.launch {
			if (serverId == null) {
				registry.add(s.name, s.url, s.username, s.password, s.browseByFolder)
			} else {
				registry.update(
					serverId, s.name, s.url, s.username, s.password, s.browseByFolder,
				)
			}
			_state.update { it.copy(saved = true) }
		}
	}
}
