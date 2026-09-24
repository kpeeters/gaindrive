package org.gaindrive.android.data.cache

import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.FlowPreview
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.SharingStarted
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.combine
import kotlinx.coroutines.flow.debounce
import kotlinx.coroutines.flow.drop
import kotlinx.coroutines.flow.stateIn
import kotlinx.coroutines.launch
import org.gaindrive.android.data.SettingsStore
import org.gaindrive.android.data.local.LocalLibrary
import org.gaindrive.android.data.local.MemberRow
import javax.inject.Inject
import javax.inject.Singleton

/**
 * Which albums and playlists are entirely on the device, whether or not anybody
 * asked for them.
 *
 * The unpinned half of the question [PinRepository.statuses] answers. That one
 * is about intent - this album was asked for, and here is how far it has got -
 * and so knows nothing about a record played straight through, which is on the
 * device just as completely and which the listings should say so about.
 *
 * A class of its own rather than more of `PinRepository` precisely because it
 * is about the things nobody pinned, which is what that class is not for.
 */
@Singleton
class StoredContainers @Inject constructor(
	private val local: LocalLibrary,
	audioCache: AudioCache,
	downloads: DownloadQueue,
	settings: SettingsStore,
	scope: CoroutineScope,
) {

	private val membership = MutableStateFlow<Map<String, List<MemberRow>>>(emptyMap())

	/** Encoded refs of the collections whose every track is stored. */
	val fullyStored: StateFlow<Set<String>> = combine(
		membership,
		audioCache.cachedKeys,
		downloads.states,
		settings.videoAudioOnly,
	) { members, cached, downloadStates, audioOnly ->
		// A finished download counts even when the cache cannot vouch for it -
		// see DownloadStates.completed on why it so often cannot. The same
		// union PinRepository.statuses takes, and for the same reason.
		val here = cached + downloadStates.completed
		collectionsFullyStored(
			membership = members.mapValues { (_, rows) ->
				rows.filter { covered(it.isVideo, audioOnly) }.map(MemberRow::refKey)
			},
			here = here,
		)
	}.stateIn(scope, SharingStarted.WhileSubscribed(SUBSCRIPTION_GRACE_MS), emptySet())

	init {
		start(scope)
	}

	@OptIn(FlowPreview::class)
	private fun start(scope: CoroutineScope) {
		scope.launch {
			// Once up front, because the first listing is drawn long before
			// anything writes to the mirror and would otherwise carry no marks
			// until it happened to.
			reload()
			local.revision.drop(1).debounce(REVISION_DEBOUNCE_MS).collect { reload() }
		}
	}

	private suspend fun reload() {
		membership.value = local.collectionMembership()
	}
}

/** Long enough to survive a rotation without reloading the whole mirror. */
private const val SUBSCRIPTION_GRACE_MS = 5_000L
