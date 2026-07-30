package org.gaindrive.android.ui.settings

import androidx.lifecycle.ViewModel
import androidx.lifecycle.viewModelScope
import dagger.hilt.android.lifecycle.HiltViewModel
import kotlinx.coroutines.flow.SharingStarted
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.combine
import kotlinx.coroutines.flow.stateIn
import kotlinx.coroutines.launch
import org.gaindrive.android.data.ServerRegistry
import org.gaindrive.android.data.SettingsStore
import org.gaindrive.android.data.model.ServerConfig
import org.gaindrive.android.data.model.ServerId
import org.gaindrive.android.data.model.ThemeMode
import javax.inject.Inject

data class SettingsUiState(
	val servers: List<ServerConfig> = emptyList(),
	val themeMode: ThemeMode = ThemeMode.AUTO,
	val mergeDuplicateAlbums: Boolean = false,
	/** Distinguishes "no servers yet" from "not loaded yet" for routing. */
	val loaded: Boolean = false,
)

@HiltViewModel
class SettingsViewModel @Inject constructor(
	private val registry: ServerRegistry,
	private val settings: SettingsStore,
) : ViewModel() {

	val state: StateFlow<SettingsUiState> =
		combine(
			registry.servers,
			settings.themeMode,
			settings.mergeDuplicateAlbums,
		) { servers, theme, merge ->
			SettingsUiState(
				servers = servers,
				themeMode = theme,
				mergeDuplicateAlbums = merge,
				loaded = true,
			)
		}.stateIn(
			scope = viewModelScope,
			started = SharingStarted.WhileSubscribed(5_000),
			initialValue = SettingsUiState(),
		)

	fun setTheme(mode: ThemeMode) = viewModelScope.launch { settings.setThemeMode(mode) }

	fun setEnabled(id: ServerId, enabled: Boolean) =
		viewModelScope.launch { registry.setEnabled(id, enabled) }

	fun remove(id: ServerId) = viewModelScope.launch { registry.remove(id) }

	/** Returns Unit, not the Job: it is passed around as a `() -> Unit` callback. */
	fun move(from: Int, to: Int) {
		viewModelScope.launch { registry.move(from, to) }
	}

	fun setMergeDuplicateAlbums(enabled: Boolean) =
		viewModelScope.launch { settings.setMergeDuplicateAlbums(enabled) }
}
