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
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.asStateFlow
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
 * What the download manager is doing, as far as anything outside needs to know.
 *
 * [failed] is tracked separately because Media3 drops failed downloads out of
 * `currentDownloads` entirely. Reading only that set made a failure
 * indistinguishable from "nothing has happened yet" — a progress ring stuck at
 * zero with no way to find out why.
 */
data class DownloadStates(
	/** Cache key to `Download.STATE_*`, for downloads not in a terminal state. */
	val active: Map<String, Int> = emptyMap(),
	val failed: Set<String> = emptySet(),
	/**
	 * Requirement flags that are *not* currently met — non-zero means every
	 * queued download is waiting rather than progressing. Usually the Wi-Fi-only
	 * setting on a metered connection.
	 */
	val notMetRequirements: Int = 0,
)

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
					val previouslyFailed = scanFailed()
					_states.update { it.copy(failed = it.failed + previouslyFailed) }
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
					if (download.state == Download.STATE_FAILED) {
						it.copy(failed = it.failed + key)
					} else {
						// A retry, or a fresh request for the same track, clears
						// the old verdict rather than leaving it to stick.
						it.copy(failed = it.failed - key)
					}
				}
				if (download.state == Download.STATE_FAILED) {
					// The only place the real cause is ever visible. Without it a
					// failed download is silent, and every cause looks the same
					// from the UI.
					Log.w(TAG, "Download failed: $key", finalException)
				}
				publish()
			}

			override fun onDownloadRemoved(
				downloadManager: DownloadManager,
				download: Download,
			) {
				_states.update { it.copy(failed = it.failed - download.request.id) }
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

	/** Failures from before this process started; the listener never replays them. */
	private suspend fun scanFailed(): Set<String> = withContext(Dispatchers.IO) {
		runCatching {
			downloads.downloadIndex.getDownloads(Download.STATE_FAILED).use { cursor ->
				buildSet { while (cursor.moveToNext()) add(cursor.download.request.id) }
			}
		}.getOrDefault(emptySet())
	}

	private fun publish() {
		_states.update {
			it.copy(
				active = downloads.currentDownloads.associate { d -> d.request.id to d.state },
				notMetRequirements = downloads.notMetRequirements,
			)
		}
	}

	private companion object {
		const val TAG = "GainDriveDownloads"
	}
}
