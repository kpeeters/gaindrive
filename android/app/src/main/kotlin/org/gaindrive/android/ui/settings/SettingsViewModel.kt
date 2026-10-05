package org.gaindrive.android.ui.settings

import androidx.lifecycle.ViewModel
import androidx.lifecycle.viewModelScope
import dagger.hilt.android.lifecycle.HiltViewModel
import kotlinx.coroutines.ExperimentalCoroutinesApi
import kotlinx.coroutines.flow.SharingStarted
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.combine
import kotlinx.coroutines.flow.distinctUntilChanged
import kotlinx.coroutines.flow.map
import kotlinx.coroutines.flow.mapLatest
import kotlinx.coroutines.flow.stateIn
import kotlinx.coroutines.launch
import org.gaindrive.android.data.ServerRegistry
import org.gaindrive.android.data.SettingsStore
import org.gaindrive.android.data.cache.AudioCache
import org.gaindrive.android.data.cache.ImageCache
import org.gaindrive.android.data.cache.PinRepository
import org.gaindrive.android.data.cache.PinStatus
import org.gaindrive.android.data.cache.PinnedItem
import org.gaindrive.android.data.model.AudioFormat
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
	val castOriginal: Boolean = false,
	val videoAudioOnly: Boolean = false,
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
	/** What a ceiling converts to; remembered while [quality] is Original. */
	val transcodeFormat: AudioFormat = AudioQuality.DEFAULT.format,
	val usedBytes: Long = 0,
	val pinnedBytes: Long = 0,
	/** The playing track's unpinned bytes, which a flush keeps. */
	val sparedBytes: Long = 0,
	/** Coil's disk cache; see [ImageCache.sizeBytes] for what it leaves out. */
	val imageBytes: Long = 0,
)

/**
 * Switches that share a slot in the outer `combine`, which is full at its typed
 * limit of five flows - the same squeeze [StoragePrefs] answers. They ride
 * together because there was room for one more value, not because they belong
 * together; all are unpacked into [SettingsUiState], which stays flat.
 */
private data class Toggles(
	val mergeDuplicateAlbums: Boolean,
	val castOriginal: Boolean,
	val videoAudioOnly: Boolean,
)

/** The stored half of [StorageUiState]; the measured half comes from the cache. */
private data class StoragePrefs(
	val maxBytes: Long,
	val cacheOnPlay: Boolean,
	val unmeteredOnly: Boolean,
	val offlineMode: Boolean,
	val quality: QualityPrefs,
)

/** The quality and the format it converts to, sharing one `combine` slot. */
private data class QualityPrefs(
	val quality: AudioQuality,
	val transcodeFormat: AudioFormat,
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
		combine(settings.audioQuality, settings.transcodeFormat, ::QualityPrefs),
		::StoragePrefs,
	)

	/**
	 * The playing track's unpinned bytes: what [flushCache] spares. Recounted
	 * when the track changes and whenever the cache's size does, which is
	 * also how its download filling in reaches the screen.
	 */
	@OptIn(ExperimentalCoroutinesApi::class)
	private val sparedBytes = combine(
		player.state.map { it.current?.ref?.encode() }.distinctUntilChanged(),
		audioCache.usedBytes,
	) { ref, _ -> ref }
		.mapLatest { ref -> ref?.let { audioCache.unpinnedBytesOf(it) } ?: 0L }

	private val storage = combine(
		storagePrefs,
		audioCache.usedBytes,
		audioCache.pinnedBytes,
		imageCache.sizeBytes,
		sparedBytes,
	) { prefs, used, pinned, images, spared ->
		StorageUiState(
			maxBytes = prefs.maxBytes,
			cacheOnPlay = prefs.cacheOnPlay,
			unmeteredOnly = prefs.unmeteredOnly,
			offlineMode = prefs.offlineMode,
			quality = prefs.quality.quality,
			transcodeFormat = prefs.quality.transcodeFormat,
			usedBytes = used,
			pinnedBytes = pinned,
			sparedBytes = spared,
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

	private val toggles = combine(
		settings.mergeDuplicateAlbums,
		settings.castOriginal,
		settings.videoAudioOnly,
		::Toggles,
	)

	val state: StateFlow<SettingsUiState> =
		combine(
			registry.servers,
			settings.themeMode,
			toggles,
			storage,
			pinnedItems,
		) { servers, theme, toggles, storage, pins ->
			SettingsUiState(
				servers = servers,
				themeMode = theme,
				mergeDuplicateAlbums = toggles.mergeDuplicateAlbums,
				castOriginal = toggles.castOriginal,
				videoAudioOnly = toggles.videoAudioOnly,
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

	/**
	 * No side effect, unlike [setAudioQuality]: nothing is stored for a cast, so
	 * there is nothing to re-fetch. It applies from the next `LOAD`.
	 */
	fun setCastOriginal(enabled: Boolean) =
		viewModelScope.launch { settings.setCastOriginal(enabled) }

	/**
	 * Pins are re-expanded, because this changes what one *covers*: with it on,
	 * a video under an album pin becomes something that can be downloaded, and
	 * with it off it stops being covered again. Without the refresh a pin would
	 * only pick the change up the next time it was touched for another reason.
	 *
	 * Playback follows separately - `PlaybackService` watches the same flow and
	 * re-resolves what is already queued.
	 */
	fun setVideoAudioOnly(enabled: Boolean) = viewModelScope.launch {
		settings.setVideoAudioOnly(enabled)
		pins.refreshDownloads()
	}

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
	 * asked for". The old copies are not deleted - they stop being protected,
	 * so eviction reclaims them when the space is next needed, and until then
	 * they keep playing.
	 *
	 * A ceiling's format is also remembered as the one to convert to, so it
	 * comes back after a spell on Original.
	 */
	fun setAudioQuality(quality: AudioQuality) = viewModelScope.launch {
		settings.setAudioQuality(quality)
		if (quality.format != AudioFormat.ORIGINAL) settings.setTranscodeFormat(quality.format)
		pins.refreshDownloads()
	}

	fun unpin(ref: ItemRef) = viewModelScope.launch { pins.unpin(ref) }

	/**
	 * Drops everything unpinned except the track playing, whose bytes the
	 * player has open.
	 *
	 * Not the whole queue, which is what this used to spare: a queue is often
	 * the very album that was cached, so the flush freed nothing while the
	 * button promised it all. The rest of the queue fetches again when
	 * reached; a next track being pre-buffered may refuse to go, and that is
	 * logged rather than fatal.
	 */
	fun flushCache() = viewModelScope.launch {
		val current = player.state.value.current?.ref?.encode()
		audioCache.flush(keepKeys = setOfNotNull(current))
	}

	/**
	 * Throws away every stored cover and portrait. No confirmation anywhere in
	 * the UI: unlike the music cache this costs nothing but the next few
	 * requests, and being able to press it twice without thinking is the point
	 * - it exists for the case where the art on screen disagrees with the
	 * server and nobody wants to work out why.
	 */
	fun clearImageCache() = viewModelScope.launch {
		imageCache.clear()
		// The clear took the pinned copies with it, and those are the ones
		// nothing will re-request on its own: a pinned album is exactly what
		// the user may next look at with no network. Offline the clear left
		// them alone, and this finds nothing missing.
		pins.refreshArt()
	}
}
