package org.gaindrive.android.data.cache

import android.content.Context
import android.net.Uri
import android.util.Log
import androidx.media3.exoplayer.offline.Download
import androidx.media3.exoplayer.offline.DownloadManager
import androidx.media3.exoplayer.offline.DownloadRequest
import androidx.media3.exoplayer.offline.DownloadService
import androidx.media3.exoplayer.scheduler.Requirements
import dagger.hilt.android.qualifiers.ApplicationContext
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.delay
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.asStateFlow
import kotlinx.coroutines.flow.collectLatest
import kotlinx.coroutines.flow.distinctUntilChanged
import kotlinx.coroutines.flow.map
import kotlinx.coroutines.flow.update
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext
import org.gaindrive.android.BuildConfig
import org.gaindrive.android.data.SettingsStore
import org.gaindrive.android.data.model.ItemRef
import org.gaindrive.android.playback.MediaDownloadService
import javax.inject.Inject
import javax.inject.Singleton

/**
 * Pinned downloads, on top of Media3's [DownloadManager].
 *
 * Downloads land in the same cache playback fills, keyed the same way, so a
 * track that was already played is finished rather than fetched again, and a
 * downloaded track is simply a cache hit when it comes to be played.
 */
@Singleton
class DownloadQueue @Inject constructor(
	@ApplicationContext private val context: Context,
	private val downloads: DownloadManager,
	settings: SettingsStore,
	private val scope: CoroutineScope,
) {

	private val _states = MutableStateFlow(DownloadStates())
	val states: StateFlow<DownloadStates> = _states.asStateFlow()

	init {
		downloads.addListener(object : DownloadManager.Listener {
			/**
			 * The index loads asynchronously, so until this fires
			 * `currentDownloads` is empty and says nothing about the downloads
			 * that were already under way when the process last died.
			 */
			override fun onInitialized(downloadManager: DownloadManager) {
				scope.launch {
					val (done, failed) = scanTerminal()
					_states.update {
						it.copy(completed = it.completed + done, failed = it.failed + failed)
					}
					publish()
				}
			}

			override fun onDownloadChanged(
				downloadManager: DownloadManager,
				download: Download,
				finalException: Exception?,
			) {
				val key = download.request.id
				_states.update {
					when (download.state) {
						Download.STATE_COMPLETED ->
							it.copy(completed = it.completed + key, failed = it.failed - key)

						Download.STATE_FAILED ->
							it.copy(completed = it.completed - key, failed = it.failed + key)

						// A retry, or a fresh request for the same track, clears
						// the old verdict rather than leaving it to stick.
						else -> it.copy(failed = it.failed - key)
					}
				}
				if (download.state == Download.STATE_FAILED) {
					// The only place the real cause is ever visible. Without it a
					// failed download is silent, and every cause looks the same
					// from the UI.
					Log.w(TAG, "Download failed: $key", finalException)
				} else if (BuildConfig.DEBUG) {
					Log.i(TAG, "Download ${stateName(download.state)}: $key")
				}
				publish()
			}

			override fun onDownloadRemoved(
				downloadManager: DownloadManager,
				download: Download,
			) {
				val key = download.request.id
				_states.update {
					it.copy(completed = it.completed - key, failed = it.failed - key)
				}
				publish()
			}

			override fun onRequirementsStateChanged(
				downloadManager: DownloadManager,
				requirements: Requirements,
				notMetRequirements: Int,
			) {
				if (BuildConfig.DEBUG && notMetRequirements != 0) {
					Log.i(TAG, "Downloads waiting; unmet requirements=$notMetRequirements")
				}
				publish()
			}
		})
		publish()

		scope.launch {
			settings.downloadUnmeteredOnly.collect { unmeteredOnly ->
				downloads.requirements = Requirements(
					if (unmeteredOnly) Requirements.NETWORK_UNMETERED else Requirements.NETWORK
				)
				publish()
			}
		}

		// Media3 pushes state changes but never progress, so a track being
		// fetched would otherwise sit at whatever percentage it held when it
		// started. Polled only while something is actually moving: a ticker
		// that runs regardless is a battery cost for nothing.
		scope.launch {
			_states.map { it.active.isNotEmpty() }
				.distinctUntilChanged()
				.collectLatest { busy ->
					while (busy) {
						delay(PROGRESS_POLL_MS)
						publish()
					}
				}
		}
	}

	/**
	 * The download id is the cache key, which is the encoded [ItemRef]. One
	 * identity for the request, the stored bytes and the media item means a
	 * download and a play can never end up as two copies of the same track.
	 */
	fun add(ref: ItemRef, url: String) {
		val request = DownloadRequest.Builder(ref.encode(), Uri.parse(url))
			.setCustomCacheKey(ref.encode())
			.build()
		// Cleared optimistically: the request is going in, so a previous verdict
		// on this key is already out of date.
		_states.update { it.copy(failed = it.failed - ref.encode()) }
		DownloadService.sendAddDownload(
			context,
			MediaDownloadService::class.java,
			request,
			/* foreground = */ false,
		)
	}

	/**
	 * Cancels the download and deletes its bytes.
	 *
	 * Media3 removes cached content along with the download and offers no way
	 * to keep one without the other, so unpinning frees the space rather than
	 * demoting the track to ordinary evictable cache. That is also the reading
	 * most people expect of "remove download", so it is not worth fighting.
	 */
	fun remove(key: String) {
		DownloadService.sendRemoveDownload(
			context,
			MediaDownloadService::class.java,
			key,
			/* foreground = */ false,
		)
	}

	/**
	 * Terminal states from before this process started, which the listener never
	 * replays. Returns completed then failed.
	 */
	private suspend fun scanTerminal(): Pair<Set<String>, Set<String>> =
		withContext(Dispatchers.IO) {
			runCatching {
				val done = mutableSetOf<String>()
				val failed = mutableSetOf<String>()
				downloads.downloadIndex
					.getDownloads(Download.STATE_COMPLETED, Download.STATE_FAILED)
					.use { cursor ->
						while (cursor.moveToNext()) {
							val download = cursor.download
							val target =
								if (download.state == Download.STATE_FAILED) failed else done
							target += download.request.id
						}
					}
				Pair<Set<String>, Set<String>>(done, failed)
			}.getOrDefault(Pair(emptySet(), emptySet()))
		}

	private fun publish() {
		_states.update {
			it.copy(
				active = downloads.currentDownloads.associate { d -> d.request.id to d.progress() },
				notMetRequirements = downloads.notMetRequirements,
			)
		}
		if (BuildConfig.DEBUG) {
			val state = _states.value
			Log.i(
				TAG,
				"active=${state.active.size} completed=${state.completed.size} " +
					"failed=${state.failed.size} notMet=${state.notMetRequirements}",
			)
		}
	}

	/**
	 * The one place Media3's seven states collapse to the two a row can show.
	 * Anything not actively fetching is waiting its turn as far as the UI is
	 * concerned, including the stopped and restarting states.
	 */
	private fun Download.progress() = TrackDownload(
		state = if (state == Download.STATE_DOWNLOADING) {
			TrackDownloadState.DOWNLOADING
		} else {
			TrackDownloadState.QUEUED
		},
		percent = percentDownloaded,
	)

	private companion object {
		const val TAG = "GainDriveDownloads"

		/** Matches the cadence Media3 uses for its own download notification. */
		const val PROGRESS_POLL_MS = 1_000L

		fun stateName(state: Int): String = when (state) {
			Download.STATE_QUEUED -> "queued"
			Download.STATE_STOPPED -> "stopped"
			Download.STATE_DOWNLOADING -> "downloading"
			Download.STATE_COMPLETED -> "completed"
			Download.STATE_FAILED -> "failed"
			Download.STATE_REMOVING -> "removing"
			Download.STATE_RESTARTING -> "restarting"
			else -> "state $state"
		}
	}
}
