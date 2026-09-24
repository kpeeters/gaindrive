package org.gaindrive.android.ui

import androidx.compose.runtime.compositionLocalOf
import androidx.lifecycle.ViewModel
import androidx.lifecycle.viewModelScope
import dagger.hilt.android.lifecycle.HiltViewModel
import kotlinx.coroutines.flow.SharingStarted
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.combine
import kotlinx.coroutines.flow.stateIn
import org.gaindrive.android.data.Connectivity
import org.gaindrive.android.data.cache.AudioCache
import org.gaindrive.android.data.cache.DownloadQueue
import org.gaindrive.android.data.cache.PinRepository
import org.gaindrive.android.data.cache.PinStatus
import org.gaindrive.android.data.cache.StoredContainers
import org.gaindrive.android.data.cache.TrackDownload
import org.gaindrive.android.data.model.ItemRef
import javax.inject.Inject

/** Whether a track can be played right now, and from where. */
enum class Availability {
	/** Held locally in full; plays with no server involved. */
	STORED,

	/** Not stored, but there is a network to fetch it over. */
	STREAMABLE,

	/** Not stored and nothing to fetch it over. */
	UNAVAILABLE,
}

/**
 * What an album or playlist row has to say about being downloaded.
 *
 * The two levels deliberately speak the same language: [Pinned] is the tick
 * (and, before it, the ring) a track gets for being asked for, [StoredOnly] is
 * the quieter dot for being here anyway. Null is not "no", it is "nothing to
 * say" - which is also the answer for a collection never opened, whose tracks
 * the mirror does not know.
 */
sealed interface ContainerMark {
	/** Asked for, with [status] saying how far it has got. */
	data class Pinned(val status: PinStatus) : ContainerMark

	/** Every track is here, but nothing is keeping them from eviction. */
	data object StoredOnly : ContainerMark
}

data class AvailabilityState(
	val online: Boolean = true,
	val storedKeys: Set<String> = emptySet(),
	val pinnedKeys: Set<String> = emptySet(),
	/**
	 * Offline because the user said so, rather than because there is no signal.
	 * Only the banner cares - everything else treats the two the same.
	 */
	val offlineByChoice: Boolean = false,
	/** Tracks being fetched right now, keyed by encoded ref. */
	val downloads: Map<String, TrackDownload> = emptyMap(),
	/** How far each *pinned* album or playlist has got, by encoded ref. */
	val pinStatuses: Map<String, PinStatus> = emptyMap(),
	/** Albums and playlists whose every track is stored, pinned or not. */
	val storedContainers: Set<String> = emptySet(),
) {

	/** What, if anything, this track's row should show about a download. */
	fun downloadOf(ref: ItemRef): TrackDownload? = downloads[ref.encode()]

	/**
	 * What, if anything, an album or playlist row should show.
	 *
	 * Takes every ref the row stands for, not one: a merged album row is one
	 * album on several servers, and having downloaded it from any of them is
	 * still having downloaded what the row names. A pin wins over the dot,
	 * being the stronger promise of the two.
	 */
	fun containerMark(refs: List<ItemRef>): ContainerMark? {
		val keys = refs.map { it.encode() }
		keys.firstNotNullOfOrNull { pinStatuses[it] }
			?.let { return ContainerMark.Pinned(it) }
		return if (keys.any { it in storedContainers }) ContainerMark.StoredOnly else null
	}

	fun of(ref: ItemRef): Availability {
		val key = ref.encode()
		return when {
			key in storedKeys -> Availability.STORED
			online -> Availability.STREAMABLE
			else -> Availability.UNAVAILABLE
		}
	}

	/** Kept on purpose: pinned, and the audio has arrived. */
	fun isDownloaded(ref: ItemRef): Boolean {
		val key = ref.encode()
		return key in pinnedKeys && key in storedKeys
	}

	/**
	 * Here because it was played, not because it was asked for.
	 *
	 * Worth its own mark rather than none at all: it is the difference between
	 * a track that will play offline and one that will not, which is exactly
	 * what someone about to lose signal wants to see. It is a weaker promise
	 * than [isDownloaded] though - eviction may reclaim it - so the two do not
	 * share a symbol.
	 */
	fun isCachedOnly(ref: ItemRef): Boolean {
		val key = ref.encode()
		return key in storedKeys && key !in pinnedKeys
	}
}

/**
 * Ambient rather than threaded through every screen.
 *
 * Nearly every row wants this and no screen has an opinion about it, so passing
 * it down by hand would mean a parameter on every list, view model and route
 * for something none of them are about.
 */
val LocalAvailability = compositionLocalOf { AvailabilityState() }

@HiltViewModel
class AvailabilityViewModel @Inject constructor(
	audioCache: AudioCache,
	pins: PinRepository,
	downloads: DownloadQueue,
	connectivity: Connectivity,
	containers: StoredContainers,
) : ViewModel() {

	/**
	 * Two groupings, because `combine` is typed only up to five flows and this
	 * wants seven. Both pair things that are about the same subject anyway, so
	 * the records read as facts rather than as a workaround for the arity.
	 */
	private data class Net(val online: Boolean, val byChoice: Boolean)

	private data class Containers(
		val pinStatuses: Map<String, PinStatus>,
		val stored: Set<String>,
	)

	private val net = combine(
		connectivity.online,
		connectivity.offlineByChoice,
	) { online, byChoice -> Net(online, byChoice) }

	private val containerState = combine(
		pins.statuses,
		containers.fullyStored,
	) { statuses, stored -> Containers(statuses, stored) }

	val state: StateFlow<AvailabilityState> =
		combine(
			net,
			audioCache.cachedKeys,
			pins.protectedKeys,
			downloads.states,
			containerState,
		) { net, cached, pinned, downloadStates, containerState ->
			AvailabilityState(
				online = net.online,
				// Union, not just the cache: a completed download is on the
				// device whether or not the cache recorded a length it can check
				// against, and gaindrive's chunked responses mean it often did
				// not.
				storedKeys = cached + downloadStates.completed,
				pinnedKeys = pinned,
				offlineByChoice = net.byChoice,
				downloads = downloadStates.active,
				pinStatuses = containerState.pinStatuses,
				storedContainers = containerState.stored,
			)
		}.stateIn(
			scope = viewModelScope,
			started = SharingStarted.WhileSubscribed(5_000),
			initialValue = AvailabilityState(),
		)
}
