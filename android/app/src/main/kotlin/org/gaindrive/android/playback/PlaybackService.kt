package org.gaindrive.android.playback

import androidx.media3.common.AudioAttributes
import androidx.media3.common.MediaItem
import androidx.media3.datasource.okhttp.OkHttpDataSource
import androidx.media3.exoplayer.ExoPlayer
import androidx.media3.exoplayer.source.DefaultMediaSourceFactory
import androidx.media3.session.LibraryResult
import androidx.media3.session.MediaLibraryService
import androidx.media3.session.MediaSession
import com.google.common.collect.ImmutableList
import com.google.common.util.concurrent.Futures
import com.google.common.util.concurrent.ListenableFuture
import dagger.hilt.android.AndroidEntryPoint
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.SupervisorJob
import kotlinx.coroutines.cancel
import kotlinx.coroutines.guava.future
import okhttp3.OkHttpClient
import org.gaindrive.android.data.LibraryRepository
import javax.inject.Inject

/**
 * Hosts the player and the media session.
 *
 * A [MediaLibraryService] rather than the simpler `MediaSessionService`: it is a
 * superset, and swapping the service base class later would rework the session
 * contract. Its browsable-tree callbacks are stubbed until Android Auto or Wear
 * justifies filling them in.
 */
@AndroidEntryPoint
class PlaybackService : MediaLibraryService() {

	@Inject
	lateinit var library: LibraryRepository

	@Inject
	lateinit var httpClient: OkHttpClient

	private val scope = CoroutineScope(SupervisorJob() + Dispatchers.Main.immediate)

	private var session: MediaLibrarySession? = null

	override fun onCreate() {
		super.onCreate()

		val player = ExoPlayer.Builder(this)
			// Streams go through the same OkHttp as everything else, so the
			// connection pool and any future caching are shared.
			.setMediaSourceFactory(
				DefaultMediaSourceFactory(
					OkHttpDataSource.Factory(httpClient)
				)
			)
			// Media3 then handles audio focus and ducking for us.
			.setAudioAttributes(AudioAttributes.DEFAULT, /* handleAudioFocus = */ true)
			.setHandleAudioBecomingNoisy(true)
			.build()

		session = MediaLibrarySession.Builder(this, player, LibraryCallback()).build()
	}

	override fun onGetSession(controllerInfo: MediaSession.ControllerInfo): MediaLibrarySession? =
		session

	/**
	 * Stopping playback should end the service rather than leave a paused
	 * notification behind with nothing queued.
	 */
	override fun onTaskRemoved(rootIntent: android.content.Intent?) {
		val player = session?.player
		if (player == null || !player.playWhenReady || player.mediaItemCount == 0) {
			stopSelf()
		}
	}

	override fun onDestroy() {
		session?.run {
			player.release()
			release()
		}
		session = null
		scope.cancel()
		super.onDestroy()
	}

	private inner class LibraryCallback : MediaLibrarySession.Callback {

		/**
		 * Items arrive carrying only a media id, and leave with a playable URI.
		 * Doing it here rather than in the UI means the stream policy has one
		 * home, and that items restored by the system — from a notification
		 * action, or after process death — get resolved too.
		 */
		override fun onAddMediaItems(
			mediaSession: MediaSession,
			controller: MediaSession.ControllerInfo,
			mediaItems: MutableList<MediaItem>,
		): ListenableFuture<MutableList<MediaItem>> = scope.future {
			val streams = library.coverUrls()
			mediaItems.mapNotNull { item ->
				val ref = item.itemRef() ?: return@mapNotNull null
				val url = streams.streamUrl(ref) ?: return@mapNotNull null
				item.buildUpon().setUri(url).build()
			}.toMutableList()
		}

		// ── Browsable tree: stubbed ─────────────────────────────────────
		//
		// Present because MediaLibraryService requires them. Filling them in is
		// what Android Auto and Wear need, and is a later decision.

		override fun onGetLibraryRoot(
			session: MediaLibrarySession,
			browser: MediaSession.ControllerInfo,
			params: LibraryParams?,
		): ListenableFuture<LibraryResult<MediaItem>> =
			Futures.immediateFuture(
				LibraryResult.ofError<MediaItem>(LibraryResult.RESULT_ERROR_NOT_SUPPORTED)
			)

		override fun onGetChildren(
			session: MediaLibrarySession,
			browser: MediaSession.ControllerInfo,
			parentId: String,
			page: Int,
			pageSize: Int,
			params: LibraryParams?,
		): ListenableFuture<LibraryResult<ImmutableList<MediaItem>>> =
			Futures.immediateFuture(
				LibraryResult.ofError<ImmutableList<MediaItem>>(
					LibraryResult.RESULT_ERROR_NOT_SUPPORTED
				)
			)
	}
}
