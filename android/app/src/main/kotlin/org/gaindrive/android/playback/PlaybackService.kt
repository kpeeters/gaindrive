package org.gaindrive.android.playback

import android.app.PendingIntent
import android.content.Intent
import androidx.core.app.NotificationManagerCompat
import androidx.media3.common.AudioAttributes
import androidx.media3.common.C
import androidx.media3.common.MediaItem
import androidx.media3.common.Player
import androidx.media3.datasource.okhttp.OkHttpDataSource
import androidx.media3.exoplayer.ExoPlayer
import androidx.media3.session.CacheBitmapLoader
import androidx.media3.session.DefaultMediaNotificationProvider
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
import kotlinx.coroutines.flow.distinctUntilChanged
import kotlinx.coroutines.flow.drop
import kotlinx.coroutines.flow.first
import kotlinx.coroutines.guava.future
import kotlinx.coroutines.launch
import kotlinx.coroutines.sync.Mutex
import kotlinx.coroutines.sync.withLock
import okhttp3.OkHttpClient
import org.gaindrive.android.MainActivity
import org.gaindrive.android.data.CaptionTracks
import org.gaindrive.android.data.Connectivity
import org.gaindrive.android.data.LibraryRepository
import org.gaindrive.android.data.SettingsStore
import org.gaindrive.android.data.StreamUrls
import org.gaindrive.android.data.cache.AudioCache
import org.gaindrive.android.data.local.LocalLibrary
import org.gaindrive.android.data.model.ItemRef
import org.gaindrive.android.di.MediaHttp
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

	/**
	 * [MediaHttp], not the shared client: the first play of anything the server
	 * has to build - a remux, or a film's soundtrack under
	 * `SettingsStore.videoAudioOnly` - sends no bytes at all until ffmpeg has
	 * finished, which is minutes for a large file and well past the shared
	 * client's 30 s read timeout.
	 */
	@Inject
	@MediaHttp
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

	@Inject
	lateinit var videoSurface: VideoSurface

	@Inject
	lateinit var watchdog: PlaybackWatchdog

	@Inject
	lateinit var equalizer: EqualizerController

	@Inject
	lateinit var local: LocalLibrary

	@Inject
	lateinit var captions: CaptionTracks

	@Inject
	lateinit var settings: SettingsStore

	@Inject
	lateinit var connectivity: Connectivity

	private val scope = CoroutineScope(SupervisorJob() + Dispatchers.Main.immediate)

	private var session: MediaLibrarySession? = null

	private var localPlayer: ExoPlayer? = null
	private var castPlayer: CastPlayer? = null

	/**
	 * Held as well as handed to the session, so [watchAudioOnly] can put a
	 * queued item back through exactly the same resolution the session's
	 * callback performed on it.
	 */
	private var libraryCallback: LibraryCallback? = null

	/**
	 * Whichever player the session is driving. Everything that observes playback
	 * asks the session rather than holding a player, because the session is the
	 * thing that survives a swap to the Chromecast and back.
	 */
	private val activePlayer: Player? get() = session?.player

	override fun onCreate() {
		super.onCreate()

		// Streams go through the same OkHttp as everything else, so the
		// connection pool is shared; AudioCache then wraps it so playing a
		// track also stores it, and a stored track plays with no network.
		// Video takes the unwrapped one - see GainDriveMediaSourceFactory.
		val network = OkHttpDataSource.Factory(httpClient)

		val player = ExoPlayer.Builder(this)
			.setMediaSourceFactory(
				GainDriveMediaSourceFactory(
					cached = audioCache.dataSourceFactory(network),
					direct = network,
				)
			)
			// Media3 then handles audio focus and ducking for us.
			.setAudioAttributes(AudioAttributes.DEFAULT, /* handleAudioFocus = */ true)
			.setHandleAudioBecomingNoisy(true)
			.build()

		localPlayer = player
		attachListeners(player)
		startScrobbleWatcher()
		// The video screen may already be waiting for this; see VideoSurface.
		videoSurface.registerPlayer(player)
		// Only the local player can stall on a stream - the Chromecast fetches
		// its own, and nothing here would see it.
		watchdog.registerPlayer(player)
		// Likewise local-only: the effect lives on this player's audio
		// session, and a receiver's equalizer is the receiver's business.
		equalizer.registerPlayer(player)

		val callback = LibraryCallback()
		libraryCallback = callback
		// CacheBitmapLoader holds the last bitmap it loaded, which is what
		// stops a decode on every notification refresh.
		session = MediaLibrarySession.Builder(this, player, callback)
			.setBitmapLoader(CacheBitmapLoader(CoilBitmapLoader(this, scope, connectivity)))
			// Set explicitly rather than left to Media3's default, so a tap
			// on the notification or the TV's Now Playing card is sure to
			// bring the app back with its transport (Play's TV review,
			// TV-PA). singleTop lands it on the running activity.
			.setSessionActivity(
				PendingIntent.getActivity(
					this,
					0,
					Intent(this, MainActivity::class.java),
					PendingIntent.FLAG_IMMUTABLE or PendingIntent.FLAG_UPDATE_CURRENT,
				)
			)
			.build()
		watchCastDevice()
		watchAudioOnly()
	}

	/**
	 * Re-resolves the queue when `videoAudioOnly` changes.
	 *
	 * Items are resolved once, when they are added, so without this the switch
	 * would only reach the track *after* the one playing - and the gesture it
	 * replaces (backing out of the video surface) acts on the film in front of
	 * you. The position is carried across, so a film picks up its soundtrack
	 * where the picture stopped.
	 *
	 * `drop(1)` because the first value is the state the queue was already
	 * resolved under; collecting it would rebuild the queue on every start.
	 */
	private fun watchAudioOnly() = scope.launch {
		settings.videoAudioOnly.distinctUntilChanged().drop(1).collect { audioOnly ->
			val player = activePlayer ?: return@collect
			val callback = libraryCallback ?: return@collect
			val items = (0 until player.mediaItemCount).map { player.getMediaItemAt(it) }
			// A queue of nothing but music is unaffected. `!= false` rather
			// than `== true`: an item the system restored from a bare media id
			// carries no extras, and not knowing is a reason to go and ask.
			if (items.none { it.isVideoOrNull() != false || it.isAudioOnlyVideo() })
				return@collect

			val index = player.currentMediaItemIndex
			val position = player.currentPosition.coerceAtLeast(0)
			val wasPlaying = player.isPlaying
			// Anything that fails to resolve keeps the URL it has, which is
			// still playable - a re-resolve is an improvement, not a repair.
			val resolved = items.map { callback.resolve(it, audioOnly) ?: it }

			// setMediaItems with an index and a position, the same shape the
			// cast handover uses: it is the one call that moves a whole queue
			// without losing where in it the user was.
			player.setMediaItems(resolved, index, position)
			player.prepare()
			if (wasPlaying) player.play()
		}
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

	/** The last state sent for [currentRef], so a restart after "stopped" is
	 * announced again and the refresh knows whether there is anything to keep
	 * alive. */
	private var reportedState: String? = null

	/** Reports leave in the order they were made. They are separate requests,
	 * and a "paused" overtaken by the "playing" before it would leave the
	 * entry wrong until the next refresh. */
	private val reportLock = Mutex()

	private fun report(ref: ItemRef, state: String, positionMs: Long, on: CoroutineScope = scope) {
		if (ref == currentRef) reportedState = state
		on.launch {
			reportLock.withLock { library.reportPlayback(ref, state, positionMs.coerceAtLeast(0)) }
		}
	}

	/**
	 * "stopped" on the way out. On its own scope, because [releaseEverything]
	 * cancels the service's straight after; and [currentRef] is cleared first,
	 * so the pause that follows finds nothing to report and cannot overtake it.
	 */
	private fun reportStoppedOnExit() {
		val ref = currentRef ?: return
		val position = activePlayer?.currentPosition ?: 0L
		currentRef = null
		if (reportedState == "stopped") return
		report(ref, "stopped", position, on = CoroutineScope(Dispatchers.IO))
	}

	private val scrobbler = object : Player.Listener {
		override fun onEvents(player: Player, events: Player.Events) {
			// Both players carry this listener; only the one the session plays
			// through speaks for the device. The other is being paused or
			// stopped as part of a handover.
			if (player !== activePlayer) return

			if (events.contains(Player.EVENT_MEDIA_ITEM_TRANSITION)) {
				val previous = currentRef
				val ref = player.currentMediaItem?.itemRef()
				// The entry is one per account per server, so a queue moving to
				// another server has to end the one it leaves behind.
				if (previous != null && previous.server != ref?.server && reportedState != "stopped")
					report(previous, "stopped", 0)
				currentRef = ref
				submitted = false
				reportedState = null
				if (ref != null)
					report(ref, if (player.playWhenReady) "starting" else "paused", player.currentPosition)
				return
			}

			val ref = currentRef ?: return
			val state = player.playbackState
			when {
				// The end of the queue, or a stop: nothing is coming out of
				// this device any more.
				events.contains(Player.EVENT_PLAYBACK_STATE_CHANGED)
					&& (state == Player.STATE_ENDED || state == Player.STATE_IDLE) -> {
					if (reportedState != "stopped") report(ref, "stopped", player.currentPosition)
				}
				// playWhenReady rather than isPlaying: buffering after a seek
				// is not the user pausing, and must not hide them from others.
				events.contains(Player.EVENT_PLAY_WHEN_READY_CHANGED)
					|| (events.contains(Player.EVENT_PLAYBACK_STATE_CHANGED) && reportedState == "stopped") -> {
					val next = when {
						reportedState == "stopped" -> if (player.playWhenReady) "starting" else "paused"
						player.playWhenReady -> "playing"
						else -> "paused"
					}
					if (next != reportedState) report(ref, next, player.currentPosition)
				}
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
	 * being prepared while the first plays - the case that matters.
	 *
	 * Worth doing while casting too: the receiver fetches from the same server,
	 * so a transcode warmed now is one it will not wait for.
	 */
	private val prewarmWatcher = object : Player.Listener {
		override fun onMediaItemTransition(mediaItem: MediaItem?, reason: Int) {
			val player = activePlayer ?: return
			val index = player.nextMediaItemIndex
			if (index == C.INDEX_UNSET) return
			val item = player.getMediaItemAt(index)
			// Not for a video being played as one. The prewarmer builds an
			// *audio* stream URL, so it would ask the server to encode a film's
			// soundtrack for bytes nothing will ever read.
			//
			// A video being played *as* audio is the opposite case, and the one
			// that most needs this: extracting a film's soundtrack is a
			// blocking transcode of a multi-gigabyte file, so landing that wait
			// on the moment the user taps play is the worst place for it.
			if (item.isVideo()) return
			val next = item.itemRef() ?: return
			scope.launch { prewarm.warm(next, audioOnlyVideo = item.isAudioOnlyVideo()) }
		}
	}

	/**
	 * Submits a completed play once the track has been listened to.
	 *
	 * Half the track, or four minutes, whichever comes first - the convention
	 * scrobbling services have used for years, and it stops a long track
	 * needing to finish before it counts.
	 */
	private fun startScrobbleWatcher() = scope.launch {
		var sinceRefresh = 0L
		while (true) {
			delay(SCROBBLE_POLL_MS)
			val player = activePlayer ?: continue
			val ref = currentRef ?: continue

			// The server drops an entry not heard from in five minutes, and a
			// single track can outlast that.
			sinceRefresh += SCROBBLE_POLL_MS
			if (sinceRefresh >= REFRESH_MS && player.playWhenReady
				&& (reportedState == "starting" || reportedState == "playing")) {
				sinceRefresh = 0
				report(ref, "playing", player.currentPosition)
			}

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
	 * Swiping the app out of recents means quit: pause whatever is playing -
	 * the cast player included, since the session holds whichever one is
	 * active - and end the service, so no playing notification outlives the
	 * app. This only covers a user-initiated task removal; a system memory
	 * kill never calls onTaskRemoved, so restore-after-restart is unaffected.
	 *
	 * The release is done here rather than left to onDestroy, because
	 * stopSelf() cannot destroy a service that still has bound clients - and
	 * [PlayerConnection]'s app-context MediaController is exactly that, with
	 * nobody left to release it once the task is gone (the process survives
	 * the swipe; the foreground service is what keeps it alive). Waiting for
	 * onDestroy left the session alive and the notification pinned to the
	 * lock screen, observed on Android 13. Releasing the session disconnects
	 * the bound controllers, after which the stopSelf() already issued can
	 * complete; the notification is cancelled separately, see
	 * [releaseEverything]. The pause still goes first, so a cast receiver
	 * hears it before the bridge is torn down.
	 */
	override fun onTaskRemoved(rootIntent: android.content.Intent?) {
		reportStoppedOnExit()
		pauseAllPlayersAndStopSelf()
		releaseEverything()
	}

	/** Idempotent: everything is null-checked and nulled, so the second run
	 * (onTaskRemoved first, then onDestroy) is a no-op. */
	private fun releaseEverything() {
		reportStoppedOnExit()
		session?.release()
		session = null
		// Media3 only cancels its notification from a later update that finds
		// the session gone. While paused the notification is no longer the
		// foreground service's, so if no such update happens to be queued it
		// outlives the release, showing a player that no longer exists.
		NotificationManagerCompat.from(this)
			.cancel(DefaultMediaNotificationProvider.DEFAULT_NOTIFICATION_ID)
		libraryCallback = null
		// Both, and by name: the session only holds whichever one was active,
		// and the other would leak its listener and its coroutine.
		castPlayer?.release()
		castPlayer = null
		videoSurface.registerPlayer(null)
		watchdog.registerPlayer(null)
		equalizer.registerPlayer(null)
		localPlayer?.release()
		localPlayer = null
		scope.cancel()
	}

	override fun onDestroy() {
		releaseEverything()
		super.onDestroy()
	}

	private companion object {
		const val SCROBBLE_POLL_MS = 5_000L
		const val SUBMIT_AFTER_MS = 4 * 60 * 1000L
		// Well inside the server's five minutes, so one lost refresh is not
		// enough to drop the entry.
		const val REFRESH_MS = 60_000L
	}

	private inner class LibraryCallback : MediaLibrarySession.Callback {

		/**
		 * Items arrive carrying only a media id, and leave with a playable URI.
		 * Doing it here rather than in the UI means the stream policy has one
		 * home, and that items restored by the system - from a notification
		 * action, or after process death - get resolved too.
		 *
		 * The cache key is set here too, and it is derived from the [ItemRef]
		 * and the quality rather than from the URL. Stream URLs carry a
		 * per-client-instance auth salt (see `API-CLIENT.md`), so the default
		 * URL-derived key would miss after every process restart - and would
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
			val audioOnly = settings.videoAudioOnly.first()
			mediaItems.mapNotNull { item -> resolve(item, audioOnly) }.toMutableList()
		}

		/**
		 * One item, from a bare media id to a playable URI.
		 *
		 * Split out of [onAddMediaItems] because re-resolving the queue after
		 * `videoAudioOnly` changes has to make exactly the same decision - a
		 * second copy of this branch is how the two would drift into resolving
		 * the same film differently.
		 */
		suspend fun resolve(item: MediaItem, audioOnly: Boolean): MediaItem? {
			val ref = item.itemRef() ?: return null
			val video = isVideo(item, ref)
			return when {
				video && !audioOnly -> resolveVideo(item, ref)
				// Everything downstream reads the item rather than the setting:
				// with the video flag cleared, the byte cache takes it, no
				// surface is drawn and it can be downloaded - all of which is
				// the point.
				video -> resolveAudio(item, ref, audioOnlyVideo = true)
				else -> resolveAudio(item, ref, audioOnlyVideo = false)
			}
		}

		private suspend fun resolveAudio(
			item: MediaItem,
			ref: ItemRef,
			audioOnlyVideo: Boolean,
		): MediaItem? {
			// Declaring the formats media3 takes as they stand, so an MP3 is
			// not re-encoded to Opus for nothing. Passed here rather than
			// defaulted in the builder: the Cast route reaches that same
			// builder, and a receiver declares none of this. It is also what
			// media3 knows and StreamUrls has no business knowing.
			val target = streamUrls.forPlayback(
				ref,
				audioOnlyVideo,
				MEDIA3_AUDIO,
			) ?: return null
			// The quality is recorded on the item on the way past, the same way
			// resolveVideo marks what it learned: it is decided here and
			// nowhere else, and the info dialog would otherwise have to
			// recompute it from inputs that move underneath a playing track.
			val marked = if (audioOnlyVideo) item.markedAsAudioOnlyVideo() else item
			return marked.withQuality(target.quality).buildUpon()
				.setUri(target.url)
				.setCustomCacheKey(target.cacheKey)
				// Set after the URI: it applies to the LocalConfiguration,
				// which only exists once there is one. Null for the
				// original, where sniffing is the only honest answer.
				.setMimeType(target.mimeType)
				// Dropped rather than kept: an item re-resolved from the video
				// path is still carrying the film's subtitle configurations,
				// and there is nothing to draw them on.
				.setSubtitleConfigurations(emptyList())
				.build()
		}

		/**
		 * No cache key, because video never enters the byte cache - see
		 * [GainDriveMediaSourceFactory], which reads the same flag to decide
		 * which data source the item loads through.
		 *
		 * Subtitles are attached here rather than later because a
		 * `SubtitleConfiguration` is part of the item ExoPlayer prepares, and
		 * adding one afterwards means preparing the source again. The lookup
		 * costs one `getVideoInfo`, in which the server runs `ffprobe`, so it
		 * is allowed to fail quietly: a film with no captions is a film, a film
		 * that would not load is not.
		 */
		private suspend fun resolveVideo(item: MediaItem, ref: ItemRef): MediaItem? {
			// Whether the declaration below will actually be honoured, which is
			// also what decides where the subtitle tracks come from. Both halves
			// are needed: without nativeSeek the server can only re-encode, and
			// no declaration changes that. A local mirror read, no network.
			val song = local.song(ref)
			val demuxedHere = item.nativeSeek() && demuxedLocally(song?.suffix)
			// Progressive only when the server hands over the file as stored:
			// by its own rule (no transcodedContentType) or by our declaration.
			// Anything else would arrive as a pipe with no Range, so it goes as
			// HLS instead.
			val asStored = demuxedHere || (item.nativeSeek()
				&& song != null && song.transcodedContentType == null)

			// The one call site that declares what this player can demux. Local
			// playback is the only route where that is true of whoever reads the
			// bytes - the cast route deliberately declares nothing.
			val target = streamUrls.forVideo(ref, asStored, MEDIA3_CONTAINERS)
				?: return null
			// Marked before it goes out, so a restored item carries the answer
			// the mirror just gave: GainDriveMediaSourceFactory reads the same
			// flag to keep this off the byte cache, and the UI reads it to
			// offer a picture.
			return item.markedAsVideo().buildUpon()
				.setUri(target.url)
				.setMimeType(target.mimeType)
				.setSubtitleConfigurations(
					captions.configurationsFor(ref, sidecarOnly = demuxedHere)
				)
				.build()
		}

		/**
		 * Items the system restored after process death carry only a media id,
		 * so the extra that would answer this is gone. Falling back to audio
		 * would resolve a film to `format=opus` and play its soundtrack under a
		 * black screen - the exact failure video support exists to remove - so
		 * the mirror is consulted instead. A local read, no network.
		 *
		 * The audio-only flag is checked first and is not redundant with it: an
		 * item resolved that way carries `isVideo = false` on purpose, so that
		 * everything downstream treats it as audio. Without this line, turning
		 * the setting back off would find a film claiming not to be one and
		 * leave it as audio for ever.
		 */
		private suspend fun isVideo(item: MediaItem, ref: ItemRef): Boolean =
			item.isAudioOnlyVideo()
				|| (item.isVideoOrNull() ?: local.song(ref)?.isVideo ?: false)

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
