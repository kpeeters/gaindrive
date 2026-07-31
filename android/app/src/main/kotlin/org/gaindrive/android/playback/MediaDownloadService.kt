package org.gaindrive.android.playback

import android.app.Notification
import androidx.media3.exoplayer.offline.Download
import androidx.media3.exoplayer.offline.DownloadManager
import androidx.media3.exoplayer.offline.DownloadService
import androidx.media3.exoplayer.scheduler.Scheduler
import androidx.media3.ui.DownloadNotificationHelper
import dagger.hilt.android.AndroidEntryPoint
import org.gaindrive.android.R
import javax.inject.Inject

/**
 * Runs pinned downloads in the foreground, with a progress notification.
 *
 * A foreground service rather than a background job because a pin is something
 * the user just asked for and is waiting on: it should not be deferred, and it
 * should be visible and cancellable while it runs.
 *
 * No [Scheduler] is installed, so downloads do not resume on their own after a
 * reboot or after their network requirement stops being met — they resume when
 * the app is next opened. Adding `PlatformScheduler` here is the fix if that
 * turns out to matter, at the cost of a boot receiver.
 */
@AndroidEntryPoint
class MediaDownloadService : DownloadService(
	FOREGROUND_NOTIFICATION_ID,
	DownloadService.DEFAULT_FOREGROUND_NOTIFICATION_UPDATE_INTERVAL,
	CHANNEL_ID,
	R.string.download_channel_name,
	/* channelDescriptionResourceId = */ 0,
) {

	// Not named `downloadManager`: Kotlin would generate a getter that clashes
	// with the getDownloadManager() this class has to override.
	@Inject
	lateinit var downloads: DownloadManager

	private val notifications by lazy { DownloadNotificationHelper(this, CHANNEL_ID) }

	override fun getDownloadManager(): DownloadManager = downloads

	override fun getScheduler(): Scheduler? = null

	override fun getForegroundNotification(
		currentDownloads: MutableList<Download>,
		notMetRequirements: Int,
	): Notification = notifications.buildProgressNotification(
		/* context = */ this,
		R.drawable.ic_download,
		/* contentIntent = */ null,
		/* message = */ null,
		currentDownloads,
		notMetRequirements,
	)

	companion object {
		const val CHANNEL_ID = "downloads"
		private const val FOREGROUND_NOTIFICATION_ID = 2
	}
}
