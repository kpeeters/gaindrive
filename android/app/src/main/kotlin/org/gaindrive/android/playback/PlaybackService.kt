package org.gaindrive.android.playback

import androidx.media3.common.AudioAttributes
import androidx.media3.common.C
import androidx.media3.common.MediaItem
import androidx.media3.common.Player
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
import kotlinx.coroutines.delay
import kotlinx.coroutines.guava.future
import kotlinx.coroutines.launch
import okhttp3.OkHttpClient
import org.gaindrive.android.data.LibraryRepository
import org.gaindrive.android.data.StreamUrls
import org.gaindrive.android.data.cache.AudioCache
import org.gaindrive.android.data.model.ItemRef
import org.gaindrive.android.playback.cast.CastBridge
import org.gaindrive.android.playback.cast.CastPlayer
import org.gaindrive.android.playback.cast.CastSession
import org.gaindrive.android.playback.cast.CastUrls
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

	@Inject
	lateinit var audioCache: AudioCache

	@Inject
	lateinit var streamUrls: StreamUrls

	@Inject
	lateinit var prewarm: TranscodePrewarmer

	@Inject
	lateinit var castSession: CastSession

	@Inject
	lateinit var castUrls: CastUrls

	@Inject
	lateinit var castBridge: CastBridge

	private val scope = CoroutineScope(SupervisorJob() + Dispatchers.Main.immediate)

	private var session: MediaLibrarySession? = null

	private var localPlayer: ExoPlayer? = null
	private var castPlayer: CastPlayer? = null

	/**
	 * Whichever player the session is driving. Everything that observes playback
	 * asks the session rather than holding a player, because the session is the
	 * thing that survives a swap to the Chromecast and back.
	 */
	private val activePlayer: Player? get() = session?.player

	override fun onCreate() {
		super.onCreate()

		val player = ExoPlayer.Builder(this)
			// Streams go through the same OkHttp as everything else, so the
			// connection pool is shared; AudioCache then wraps it so playing a
			// track also stores it, and a stored track plays with no network.
			.setMediaSourceFactory(
				DefaultMediaSourceFactory(
					audioCache.dataSourceFactory(OkHttpDataSource.Factory(httpClient))
				)
			)
			// Media3 then handles audio focus and ducking for us.
			.setAudioAttributes(AudioAttributes.DEFAULT, /* handleAudioFocus = */ true)
			.setHandleAudioBecomingNoisy(true)
			.build()

		localPlayer = player
		attachListeners(player)
		startScrobbleWatcher()

		session = MediaLibrarySession.Builder(this, player, LibraryCallback()).build()
		watchCastDevice()
	}

	private fun attachListeners(player: Player) {
		player.addListener(scrobbler)
		player.addListener(prewarmWatcher)
	}

	// ── Handing playback to the Chromecast and back ─────────────────────────

	/**
	 * Follows the cast session, swapping the session's player as a device is
	 * selected or dropped.
	 *
	 * The queue and position travel with the swap, which is the whole reason the
	 * app owns the queue rather than letting either player be its home.
	 */
	private fun watchCastDevice() = scope.launch {
		castSession.device.collect { device ->
			if (device != null) goRemote() else goLocal()
		}
	}

	private fun goRemote() {
		val session = session ?: return
		val local = localPlayer ?: return
		if (session.player === castPlayer && castPlayer != null) return

		val remote = castPlayer ?: CastPlayer(castSession, castUrls, scope)
			.also {
				castPlayer = it
				attachListeners(it)
			}

		val items = (0 until local.mediaItemCount).map { local.getMediaItemAt(it) }
		val startIndex = local.currentMediaItemIndex
		val startPosition = local.currentPosition.coerceAtLeast(0)
		val wasPlaying = local.isPlaying

		// Pause rather than stop: the local player keeps the queue it had, and
		// leaving it running would play the track twice, once in each room.
		local.pause()

		session.player = remote
		if (items.isNotEmpty()) {
			remote.setMediaItems(items, startIndex, startPosition)
			remote.prepare()
			if (wasPlaying) remote.play()
		}
	}

	private fun goLocal() {
		val session = session ?: return
		val remote = castPlayer ?: return
		val local = localPlayer ?: return
		if (session.player === local) return

		val items = (0 until remote.mediaItemCount).map { remote.getMediaItemAt(it) }
		val startIndex = remote.currentMediaItemIndex
		val startPosition = remote.currentPosition.coerceAtLeast(0)

		remote.stop()
		session.player = local
		// Nothing is pulling from it any more, and it holds a Wi-Fi lock.
		castBridge.stop()
		if (items.isNotEmpty()) {
			local.setMediaItems(items, startIndex, startPosition)
			local.prepare()
		}
		// Deliberately not resumed. Stopping a cast is the user leaving the
		// speakers they were listening on, and having the phone take over out
		// loud is rarely what they meant.
	}

	// ── Scrobbling ──────────────────────────────────────────────────────────
	//
	// Lives in the service, not the UI: a play must be reported whether or not
	// anything is on screen, and the service is what survives backgrounding.

	private var currentRef: ItemRef? = null
	private var submitted = false

	private val scrobbler = object : Player.Listener {
		override fun onMediaItemTransition(mediaItem: MediaItem?, reason: Int) {
			val ref = mediaItem?.itemRef()
			currentRef = ref
			submitted = false
			if (ref != null) {
				scope.launch { library.scrobble(ref, submission = false) }
			}
		}
	}

	// ── Pre-warming ─────────────────────────────────────────────────────────
	//
	// Its own listener rather than more code inside the scrobbler: the two
	// answer different questions, and the scrobbler deliberately knows nothing
	// about the queue.

	/**
	 * On every track change, asks the server to prepare the one after it.
	 *
	 * It asks the session for the queue rather than closing over a player,
	 * because [Player.Listener] is handed only the item that just started and
	 * the player underneath may since have become the Chromecast. The first
	 * transition fires when playback begins, so the second track of a queue is
	 * being prepared while the first plays — the case that matters.
	 *
	 * Worth doing while casting too: the receiver fetches from the same server,
	 * so a transcode warmed now is one it will not wait for.
	 */
	private val prewarmWatcher = object : Player.Listener {
		override fun onMediaItemTransition(mediaItem: MediaItem?, reason: Int) {
			val player = activePlayer ?: return
			val index = player.nextMediaItemIndex
			if (index == C.INDEX_UNSET) return
			val next = player.getMediaItemAt(index).itemRef() ?: return
			scope.launch { prewarm.warm(next) }
		}
	}

	/**
	 * Submits a completed play once the track has been listened to.
	 *
	 * Half the track, or four minutes, whichever comes first — the convention
	 * scrobbling services have used for years, and it stops a long track
	 * needing to finish before it counts.
	 */
	private fun startScrobbleWatcher() = scope.launch {
		while (true) {
			delay(SCROBBLE_POLL_MS)
			val player = activePlayer ?: continue
			val ref = currentRef ?: continue
			if (submitted || !player.isPlaying) continue

			val duration = player.duration
			val position = player.currentPosition
			if (duration <= 0) continue

			if (position >= duration / 2 || position >= SUBMIT_AFTER_MS) {
				submitted = true
				library.scrobble(ref, submission = true)
			}
		}
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
		session?.release()
		session = null
		// Both, and by name: the session only holds whichever one was active,
		// and the other would leak its listener and its coroutine.
		castPlayer?.release()
		castPlayer = null
		localPlayer?.release()
		localPlayer = null
		scope.cancel()
		super.onDestroy()
	}

	private companion object {
		const val SCROBBLE_POLL_MS = 5_000L
		const val SUBMIT_AFTER_MS = 4 * 60 * 1000L
	}

	private inner class LibraryCallback : MediaLibrarySession.Callback {

		/**
		 * Items arrive carrying only a media id, and leave with a playable URI.
		 * Doing it here rather than in the UI means the stream policy has one
		 * home, and that items restored by the system — from a notification
		 * action, or after process death — get resolved too.
		 *
		 * The cache key is set here too, and it is derived from the [ItemRef]
		 * and the quality rather than from the URL. Stream URLs carry a
		 * per-client-instance auth salt (see `API-CLIENT.md`), so the default
		 * URL-derived key would miss after every process restart — and would
		 * not match what a download stored.
		 *
		 * [StreamUrls.forPlayback] pairs the URL, the key and the MIME type, so
		 * playback and downloads cannot end up disagreeing about what quality
		 * the stored bytes are.
		 */
		override fun onAddMediaItems(
			mediaSession: MediaSession,
			controller: MediaSession.ControllerInfo,
			mediaItems: MutableList<MediaItem>,
		): ListenableFuture<MutableList<MediaItem>> = scope.future {
			mediaItems.mapNotNull { item ->
				val ref = item.itemRef() ?: return@mapNotNull null
				val target = streamUrls.forPlayback(ref) ?: return@mapNotNull null
				item.buildUpon()
					.setUri(target.url)
					.setCustomCacheKey(target.cacheKey)
					// Set after the URI: it applies to the LocalConfiguration,
					// which only exists once there is one. Null for the
					// original, where sniffing is the only honest answer.
					.setMimeType(target.mimeType)
					.build()
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
