package org.gaindrive.android.playback

import android.app.Notification
import androidx.core.app.NotificationCompat
import androidx.media3.exoplayer.offline.Download
import androidx.media3.exoplayer.offline.DownloadManager
import androidx.media3.exoplayer.offline.DownloadService
import androidx.media3.exoplayer.scheduler.Scheduler
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

	override fun getDownloadManager(): DownloadManager = downloads

	override fun getScheduler(): Scheduler? = null

	/**
	 * Built by hand rather than with Media3's `DownloadNotificationHelper`,
	 * which is not in any artifact this app depends on. Doing it here is a
	 * handful of lines and says exactly what we want it to say — including the
	 * waiting-for-Wi-Fi case, which the helper renders as a bare "waiting".
	 *
	 * The channel itself is created by [DownloadService] from the id and name
	 * passed to its constructor.
	 */
	override fun getForegroundNotification(
		currentDownloads: MutableList<Download>,
		notMetRequirements: Int,
	): Notification {
		val active = currentDownloads.filter { it.state == Download.STATE_DOWNLOADING }
		val percent = averagePercent(active)

		return NotificationCompat.Builder(this, CHANNEL_ID)
			.setSmallIcon(R.drawable.ic_download)
			.setContentTitle(
				if (active.isEmpty()) "Waiting to download" else "Downloading music"
			)
			.setContentText(statusLine(currentDownloads.size, notMetRequirements))
			.setProgress(100, percent ?: 0, percent == null)
			.setOngoing(true)
			// Low: a download the user started is worth showing, not worth
			// interrupting whatever they moved on to.
			.setPriority(NotificationCompat.PRIORITY_LOW)
			.build()
	}

	/**
	 * Null when nothing is downloading or no track has reported progress —
	 * rendered as an indeterminate bar rather than a misleading 0%.
	 */
	private fun averagePercent(active: List<Download>): Int? {
		// Media3 reports an unset percentage as a negative number; filtering on
		// the sign rather than on C.PERCENTAGE_UNSET keeps this true whichever
		// sentinel it uses.
		val known = active.map { it.percentDownloaded }.filter { it >= 0f }
		if (known.isEmpty()) return null
		return known.average().toInt().coerceIn(0, 100)
	}

	private fun statusLine(remaining: Int, notMetRequirements: Int): String = when {
		// The one requirement this app sets, so it is worth naming rather than
		// leaving the user to guess why nothing is happening.
		notMetRequirements != 0 -> "Waiting for Wi-Fi"
		remaining == 1 -> "1 track left"
		else -> "$remaining tracks left"
	}

	companion object {
		const val CHANNEL_ID = "downloads"
		private const val FOREGROUND_NOTIFICATION_ID = 2
	}
}
