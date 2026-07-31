package org.gaindrive.android.data.cache

import android.content.Context
import android.net.Uri
import androidx.media3.exoplayer.offline.Download
import androidx.media3.exoplayer.offline.DownloadManager
import androidx.media3.exoplayer.offline.DownloadRequest
import androidx.media3.exoplayer.offline.DownloadService
import androidx.media3.exoplayer.scheduler.Requirements
import dagger.hilt.android.qualifiers.ApplicationContext
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.asStateFlow
import kotlinx.coroutines.launch
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
	scope: CoroutineScope,
) {

	private val _inProgress = MutableStateFlow<Set<String>>(emptySet())

	/** Cache keys with a download running or waiting; the UI shows these busy. */
	val inProgress: StateFlow<Set<String>> = _inProgress.asStateFlow()

	init {
		downloads.addListener(object : DownloadManager.Listener {
			override fun onDownloadChanged(
				downloadManager: DownloadManager,
				download: Download,
				finalException: Exception?,
			) = publish()

			override fun onDownloadRemoved(
				downloadManager: DownloadManager,
				download: Download,
			) = publish()
		})
		publish()

		scope.launch {
			settings.downloadUnmeteredOnly.collect { unmeteredOnly ->
				downloads.requirements = Requirements(
					if (unmeteredOnly) Requirements.NETWORK_UNMETERED else Requirements.NETWORK
				)
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

	private fun publish() {
		_inProgress.value = downloads.currentDownloads.map { it.request.id }.toSet()
	}
}
