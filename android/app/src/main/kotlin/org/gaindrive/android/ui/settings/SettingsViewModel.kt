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
import org.gaindrive.android.data.cache.ImageCache
import org.gaindrive.android.data.cache.PinRepository
import org.gaindrive.android.data.cache.PinStatus
import org.gaindrive.android.data.cache.PinnedItem
import org.gaindrive.android.data.model.AudioQuality
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
	val offlineMode: Boolean = false,
	val quality: AudioQuality = AudioQuality.DEFAULT,
	val usedBytes: Long = 0,
	val pinnedBytes: Long = 0,
	/** Coil's disk cache; see [ImageCache.sizeBytes] for what it leaves out. */
	val imageBytes: Long = 0,
)

/** The stored half of [StorageUiState]; the measured half comes from the cache. */
private data class StoragePrefs(
	val maxBytes: Long,
	val cacheOnPlay: Boolean,
	val unmeteredOnly: Boolean,
	val offlineMode: Boolean,
	val quality: AudioQuality,
)

@HiltViewModel
class SettingsViewModel @Inject constructor(
	private val registry: ServerRegistry,
	private val settings: SettingsStore,
	private val audioCache: AudioCache,
	private val imageCache: ImageCache,
	private val pins: PinRepository,
	private val player: PlayerConnection,
) : ViewModel() {

	// Two stages, because `combine` is only typed up to five flows and this
	// needs six.
	private val storagePrefs = combine(
		settings.cacheMaxBytes,
		settings.cacheOnPlay,
		settings.downloadUnmeteredOnly,
		settings.offlineMode,
		settings.audioQuality,
		::StoragePrefs,
	)

	private val storage = combine(
		storagePrefs,
		audioCache.usedBytes,
		audioCache.pinnedBytes,
		imageCache.sizeBytes,
	) { prefs, used, pinned, images ->
		StorageUiState(
			maxBytes = prefs.maxBytes,
			cacheOnPlay = prefs.cacheOnPlay,
			unmeteredOnly = prefs.unmeteredOnly,
			offlineMode = prefs.offlineMode,
			quality = prefs.quality,
			usedBytes = used,
			pinnedBytes = pinned,
			imageBytes = images,
		)
	}

	init {
		// Nothing else reads Coil's disk cache, so it has no size until asked.
		viewModelScope.launch { imageCache.refreshSize() }
	}

	private val pinnedItems = pins.pins.map { pins.describe(it) }

	/**
	 * Kept beside [state] rather than inside it: the combine there is already at
	 * five flows, and this changes on its own schedule as downloads progress.
	 */
	val pinStatuses: StateFlow<Map<String, PinStatus>> = pins.statuses

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

	fun setOfflineMode(enabled: Boolean) =
		viewModelScope.launch { settings.setOfflineMode(enabled) }

	/**
	 * Existing downloads are re-fetched at the new quality, since the bytes on
	 * the device are the old one and a pin means "keep this, at the quality I
	 * asked for". The old copies are not deleted — they stop being protected,
	 * so eviction reclaims them when the space is next needed, and until then
	 * they keep playing.
	 */
	fun setAudioQuality(quality: AudioQuality) = viewModelScope.launch {
		settings.setAudioQuality(quality)
		pins.refreshDownloads()
	}

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

	/**
	 * Throws away every stored cover and portrait. No confirmation anywhere in
	 * the UI: unlike the music cache this costs nothing but the next few
	 * requests, and being able to press it twice without thinking is the point
	 * — it exists for the case where the art on screen disagrees with the
	 * server and nobody wants to work out why.
	 */
	fun clearImageCache() = viewModelScope.launch { imageCache.clear() }
}
