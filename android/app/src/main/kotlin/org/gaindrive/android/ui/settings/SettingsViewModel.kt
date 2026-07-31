package org.gaindrive.android.ui.settings

import androidx.lifecycle.ViewModel
import androidx.lifecycle.viewModelScope
import dagger.hilt.android.lifecycle.HiltViewModel
import kotlinx.coroutines.flow.SharingStarted
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.combine
import kotlinx.coroutines.flow.map
import kotlinx.coroutines.flow.stateIn
import kotlinx.coroutines.launch
import org.gaindrive.android.data.ServerRegistry
import org.gaindrive.android.data.SettingsStore
import org.gaindrive.android.data.cache.AudioCache
import org.gaindrive.android.data.cache.PinRepository
import org.gaindrive.android.data.cache.PinnedItem
import org.gaindrive.android.data.model.ItemRef
import org.gaindrive.android.data.model.ServerConfig
import org.gaindrive.android.data.model.ServerId
import org.gaindrive.android.data.model.ThemeMode
import org.gaindrive.android.playback.PlayerConnection
import javax.inject.Inject

data class SettingsUiState(
	val servers: List<ServerConfig> = emptyList(),
	val themeMode: ThemeMode = ThemeMode.AUTO,
	val mergeDuplicateAlbums: Boolean = false,
	val storage: StorageUiState = StorageUiState(),
	val pins: List<PinnedItem> = emptyList(),
	/** Distinguishes "no servers yet" from "not loaded yet" for routing. */
	val loaded: Boolean = false,
)

/** Grouped so the state flows stay within `combine`'s typed arities. */
data class StorageUiState(
	val maxBytes: Long = SettingsStore.DEFAULT_CACHE_BYTES,
	val cacheOnPlay: Boolean = true,
	val unmeteredOnly: Boolean = true,
	val usedBytes: Long = 0,
	val pinnedBytes: Long = 0,
)

@HiltViewModel
class SettingsViewModel @Inject constructor(
	private val registry: ServerRegistry,
	private val settings: SettingsStore,
	private val audioCache: AudioCache,
	private val pins: PinRepository,
	private val player: PlayerConnection,
) : ViewModel() {

	private val storage = combine(
		settings.cacheMaxBytes,
		settings.cacheOnPlay,
		settings.downloadUnmeteredOnly,
		audioCache.usedBytes,
		audioCache.pinnedBytes,
		::StorageUiState,
	)

	private val pinnedItems = pins.pins.map { pins.describe(it) }

	val state: StateFlow<SettingsUiState> =
		combine(
			registry.servers,
			settings.themeMode,
			settings.mergeDuplicateAlbums,
			storage,
			pinnedItems,
		) { servers, theme, merge, storage, pins ->
			SettingsUiState(
				servers = servers,
				themeMode = theme,
				mergeDuplicateAlbums = merge,
				storage = storage,
				pins = pins,
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

	fun setCacheMaxBytes(bytes: Long) =
		viewModelScope.launch { settings.setCacheMaxBytes(bytes) }

	fun setCacheOnPlay(enabled: Boolean) =
		viewModelScope.launch { settings.setCacheOnPlay(enabled) }

	fun setDownloadUnmeteredOnly(enabled: Boolean) =
		viewModelScope.launch { settings.setDownloadUnmeteredOnly(enabled) }

	fun unpin(ref: ItemRef) = viewModelScope.launch { pins.unpin(ref) }

	/**
	 * Drops everything unpinned.
	 *
	 * The whole queue is spared, not just the track playing: the next track is
	 * likely to be part-cached already, and pulling that out from under the
	 * player would restart its download for no gain.
	 */
	fun flushCache() = viewModelScope.launch {
		val inUse = player.state.value.queue.mapNotNull { it.ref?.encode() }.toSet()
		audioCache.flush(keepKeys = inUse)
	}
}
