package org.gaindrive.android.playback.cast

import android.os.Looper
import android.util.Log
import androidx.media3.common.C
import androidx.media3.common.Format
import androidx.media3.common.MediaItem
import androidx.media3.common.MimeTypes
import androidx.media3.common.Player
import androidx.media3.common.SimpleBasePlayer
import androidx.media3.common.TrackGroup
import androidx.media3.common.Tracks
import com.google.common.util.concurrent.Futures
import com.google.common.util.concurrent.ListenableFuture
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.Job
import kotlinx.coroutines.launch
import org.gaindrive.android.playback.castSource
import org.gaindrive.android.playback.itemRef

/**
 * A Media3 [Player] backed by a Chromecast.
 *
 * `SimpleBasePlayer` exists for exactly this shape of target: commands go out
 * asynchronously and state comes back on its own schedule, so the player
 * publishes a [State] snapshot rather than owning a timeline. It swaps onto the
 * existing `MediaSession` with `setPlayer`, and because the UI only ever holds a
 * `MediaController`, nothing on screen learns that playback went remote.
 *
 * **The app owns the queue.** The receiver is only ever told about one track:
 * when it reports `IDLE`/`FINISHED`, this class loads the next one. That costs a
 * gap between tracks, and buys a queue that is the same object whether playback
 * is local or remote, which is what makes swapping players at any moment safe.
 *
 * Everything here runs on the application looper, which is also where the status
 * flow is collected, so no field needs guarding.
 */
class CastPlayer(
	private val session: CastSession,
	private val castUrls: CastUrls,
	private val scope: CoroutineScope,
) : SimpleBasePlayer(Looper.getMainLooper()) {

	/**
	 * A queue entry. The uid is assigned once and kept for the entry's life:
	 * Media3 decides item identity by uid, so reusing the index would make a
	 * removal look like every following track being replaced.
	 */
	private class Entry(val uid: Long, val item: MediaItem)

	private var entries: List<Entry> = emptyList()
	private var nextUid = 0L
	private var index = 0
	private var playWhenReady = true

	/** True once the last track has finished; cleared by anything that loads. */
	private var ended = false

	private var status = CastStatus()

	/**
	 * The session whose FINISHED we have already acted on, so the repeat pushes a
	 * receiver sends while idle cannot walk the queue forward.
	 */
	private var advancedFrom = 0

	private var loadJob: Job? = null

	/** The queue entry the current subtitle selection belongs to; see [carriedCaptions]. */
	private var captionedUid = -1L

	/**
	 * Dispatched rather than immediate on purpose. The application scope uses
	 * `Main.immediate`, which would run this collector inline during
	 * construction — the flow has a current value the moment it is collected —
	 * and that would call `invalidateState()` on a player the caller does not
	 * hold a reference to yet.
	 */
	private val watcher = scope.launch(Dispatchers.Main) {
		session.status.collect(::onStatus)
	}

	// ── State ───────────────────────────────────────────────────────────────

	override fun getState(): State {
		val builder = State.Builder()
			.setAvailableCommands(COMMANDS)
			.setPlaybackState(playbackState())
			.setPlayWhenReady(playWhenReady, Player.PLAY_WHEN_READY_CHANGE_REASON_USER_REQUEST)
			.setPlaylist(entries.mapIndexed(::itemData))
		if (entries.isNotEmpty()) {
			builder.setCurrentMediaItemIndex(index)
			builder.setContentPositionMs(positionSupplier())
		}
		return builder.build()
	}

	private fun itemData(position: Int, entry: Entry): MediaItemData {
		// The receiver's own figure wins for the track playing — it read the
		// file — and the server's estimate covers the rest of the queue.
		val durationUs = when {
			position == index && status.duration > 0f ->
				(status.duration * 1_000_000).toLong()

			else -> entry.item.mediaMetadata.durationMs
				?.takeIf { it > 0 }
				?.let { it * 1000 }
				?: C.TIME_UNSET
		}
		return MediaItemData.Builder(entry.uid)
			.setMediaItem(entry.item)
			.setMediaMetadata(entry.item.mediaMetadata)
			.setDurationUs(durationUs)
			.setIsSeekable(true)
			.setIsDynamic(false)
			// Only for the track playing. Tracks carry which of them is
			// selected, and the receiver has one selection — claiming it for a
			// queued item would tick a subtitle in a film nobody has reached.
			.setTracks(if (position == index) captionTracks(entry.item) else Tracks.EMPTY)
			.build()
	}

	/**
	 * The film's subtitle tracks as Media3 describes them, so the same picker
	 * that works locally works while casting.
	 *
	 * Manufactured rather than reported: the receiver renders the captions and
	 * tells us only which trackIds are on, so there is nothing to read them
	 * back from. What makes that safe is that both ends are built from
	 * [captionConfigs] in its order — position i here is the index the picker
	 * publishes and trackId i+1 on the wire.
	 */
	/**
	 * The subtitle selection to re-send with this load: the one in force if
	 * [entry] is the item already loaded, and none if it is a different one.
	 *
	 * A seek is a whole fresh LOAD, so without this every seek would turn the
	 * subtitles off — the same fault, and the same fix, as `player.captionIndex`
	 * in the web client. Moving to another film deliberately starts clean:
	 * nothing here knows the viewer's language, and a track number means
	 * something different in the next film.
	 */
	private fun carriedCaptions(entry: Entry): List<Int> {
		val carried =
			if (entry.uid == captionedUid) session.loaded.value?.activeTrackIds.orEmpty()
			else emptyList()
		captionedUid = entry.uid
		return carried
	}

	private fun captionTracks(item: MediaItem): Tracks {
		// The receiver's own answer when it has given one, and what we asked
		// for until then. A receiver that never echoes `activeTrackIds` would
		// otherwise leave the picker showing nothing selected however many
		// times it was tapped — the exact shape of the local-playback fault
		// this feature followed, and not one to reproduce remotely.
		val active = status.activeTrackIds
			?: session.loaded.value?.activeTrackIds.orEmpty()
		val groups = item.captionConfigs().mapIndexed { i, config ->
			val id = "caption:${i + 1}"
			val format = Format.Builder()
				.setId(id)
				.setSampleMimeType(config.mimeType ?: MimeTypes.TEXT_VTT)
				// PlayerConnection labels a track `label ?: language`, so a
				// format without either shows up as "Track n".
				.setLabel(config.label)
				.setLanguage(config.language)
				.setRoleFlags(C.ROLE_FLAG_SUBTITLE)
				.build()
			Tracks.Group(
				TrackGroup(id, format),
				/* adaptiveSupported= */ false,
				intArrayOf(C.FORMAT_HANDLED),
				booleanArrayOf(active.contains(i + 1)),
			)
		}
		return if (groups.isEmpty()) Tracks.EMPTY else Tracks(groups)
	}

	/**
	 * Position comes from the receiver, which reports about once a second. It is
	 * extrapolated while playing so the seek bar moves smoothly between pushes —
	 * and corrected by the next one, which is the right way round for a clock we
	 * do not own.
	 */
	private fun positionSupplier(): PositionSupplier {
		val known = (status.currentTime * 1000).toLong().coerceAtLeast(0)
		return if (status.playerState == CastPlayerState.PLAYING) {
			PositionSupplier.getExtrapolating(known, 1f)
		} else {
			PositionSupplier.getConstant(known)
		}
	}

	private fun playbackState(): Int = when {
		entries.isEmpty() -> Player.STATE_IDLE
		ended -> Player.STATE_ENDED
		status.playerState == CastPlayerState.PLAYING ||
			status.playerState == CastPlayerState.PAUSED -> Player.STATE_READY
		// Anything else — BUFFERING, LOADING, or IDLE with a LOAD in flight —
		// is work in progress, and saying READY would let the UI claim a
		// position that does not exist yet.
		else -> Player.STATE_BUFFERING
	}

	private fun onStatus(fresh: CastStatus) {
		status = fresh
		if (fresh.isIdleFinished &&
			fresh.mediaSessionId != 0 &&
			fresh.mediaSessionId != advancedFrom
		) {
			advancedFrom = fresh.mediaSessionId
			advance()
		}
		invalidateState()
	}

	private fun advance() {
		if (index + 1 < entries.size) {
			index++
			loadCurrent(0)
		} else {
			ended = true
		}
	}

	// ── Commands ────────────────────────────────────────────────────────────

	override fun handleSetMediaItems(
		mediaItems: List<MediaItem>,
		startIndex: Int,
		startPositionMs: Long,
	): ListenableFuture<*> {
		entries = mediaItems.map { Entry(nextUid++, it) }
		index = if (startIndex == C.INDEX_UNSET) 0 else startIndex.coerceIn(0, maxOf(0, entries.size - 1))
		ended = false
		loadCurrent(if (startPositionMs == C.TIME_UNSET) 0 else startPositionMs)
		return Futures.immediateVoidFuture()
	}

	override fun handleAddMediaItems(
		index: Int,
		mediaItems: List<MediaItem>,
	): ListenableFuture<*> {
		val added = mediaItems.map { Entry(nextUid++, it) }
		val at = index.coerceIn(0, entries.size)
		entries = entries.subList(0, at) + added + entries.subList(at, entries.size)
		// An insert before the current track shifts it along; the receiver is
		// playing the same audio either way, so nothing is loaded.
		if (at <= this.index) this.index += added.size
		// A queue that had run out has somewhere to go again.
		if (ended && this.index + 1 < entries.size) {
			ended = false
			this.index++
			loadCurrent(0)
		}
		return Futures.immediateVoidFuture()
	}

	override fun handleRemoveMediaItems(fromIndex: Int, toIndex: Int): ListenableFuture<*> {
		val from = fromIndex.coerceIn(0, entries.size)
		val to = toIndex.coerceIn(from, entries.size)
		entries = entries.subList(0, from) + entries.subList(to, entries.size)
		val removed = to - from
		when {
			// The track playing was removed. Nothing else can sensibly continue,
			// so stop rather than jump somewhere the user did not ask for.
			index in from until to -> {
				index = from.coerceAtMost(maxOf(0, entries.size - 1))
				if (entries.isEmpty()) {
					session.stopPlayback()
					ended = false
				} else {
					loadCurrent(0)
				}
			}

			index >= to -> index -= removed
		}
		return Futures.immediateVoidFuture()
	}

	override fun handleSetPlayWhenReady(playWhenReady: Boolean): ListenableFuture<*> {
		this.playWhenReady = playWhenReady
		if (playWhenReady) session.play() else session.pause()
		return Futures.immediateVoidFuture()
	}

	override fun handleSeek(
		mediaItemIndex: Int,
		positionMs: Long,
		seekCommand: Int,
	): ListenableFuture<*> {
		if (entries.isEmpty()) return Futures.immediateVoidFuture()
		val target = if (mediaItemIndex == C.INDEX_UNSET) index else mediaItemIndex
		val position = if (positionMs == C.TIME_UNSET) 0 else positionMs

		if (target != index) {
			index = target.coerceIn(entries.indices)
			ended = false
			loadCurrent(position)
		} else {
			// Within the current track the receiver seeks itself, using the
			// file's own index. Re-loading would restart buffering for nothing.
			ended = false
			session.seek(position / 1000f)
		}
		return Futures.immediateVoidFuture()
	}

	/** The LOAD is the preparation; there is nothing to do in advance of it. */
	override fun handlePrepare(): ListenableFuture<*> = Futures.immediateVoidFuture()

	override fun handleStop(): ListenableFuture<*> {
		loadJob?.cancel()
		session.stopPlayback()
		return Futures.immediateVoidFuture()
	}

	override fun handleRelease(): ListenableFuture<*> {
		loadJob?.cancel()
		watcher.cancel()
		return Futures.immediateVoidFuture()
	}

	/**
	 * Resolves the current entry's stream URL and hands it to the receiver.
	 *
	 * [CastUrls] builds it the same way local playback would — same quality,
	 * same per-server bitrate cap — and then decides whether the receiver
	 * fetches from the server or through the bridge.
	 *
	 * When it cannot build one at all, the entry is skipped rather than left to
	 * stall. That is a video the server could only convert as it plays, which
	 * the receiver would not be able to seek; the UI declines to offer such a
	 * thing and refuses to enqueue one, but neither guard covers connecting a
	 * device to a queue that already holds it.
	 */
	private fun loadCurrent(positionMs: Long) {
		val entry = entries.getOrNull(index) ?: return
		val ref = entry.item.itemRef() ?: return
		val source = entry.item.castSource()
		ended = false
		loadJob?.cancel()
		loadJob = scope.launch {
			val target = castUrls.forCast(ref, source) ?: run {
				Log.i(TAG, "nothing castable for $ref, skipping")
				advance()
				return@launch
			}
			val metadata = entry.item.mediaMetadata
			session.load(
				CastMedia(
					url = target.url,
					mimeType = target.mimeType,
					durationSeconds = metadata.durationMs
						?.takeIf { it > 0 }
						?.let { it / 1000.0 },
					startSeconds = positionMs / 1000f,
					title = metadata.title?.toString(),
					artist = metadata.artist?.toString(),
					album = metadata.albumTitle?.toString(),
					artworkUrl = castUrls.artworkFor(
						metadata.artworkUri?.toString(),
						bridged = target.bridged,
					),
					isVideo = source.isVideo,
					route = target.route,
					quality = target.quality,
					// Always declared, never only when one is chosen: the
					// receiver will not accept a trackId the LOAD did not
					// mention, so omitting them would make the picker work
					// before playback and silently fail during it.
					captions = castUrls.captionsFor(
						entry.item.captionConfigs(),
						bridged = target.bridged,
					),
					// Carried across the reload a seek performs, which is the
					// only way the receiver is ever told twice about one film.
					//
					// Keyed on the queue entry, not on the URL: a bridged load
					// mints a fresh key every time, so comparing URLs would
					// call every seek a new film and drop the subtitle the
					// viewer had chosen.
					activeTrackIds = carriedCaptions(entry),
				)
			)
		}
	}

	private companion object {
		const val TAG = "GainDriveCast"

		/**
		 * The subtitle configurations attached to an item, and **the one
		 * ordered list captions are numbered by**: position i is index i in
		 * `PlayerState.textTracks`, the group at i in [captionTracks], and
		 * trackId i+1 on the wire. Deriving either end from a second lookup
		 * would be two numberings that agree until they do not — the same
		 * class of fault as the TrackGroup identity failure in `VideoSurface`.
		 *
		 * They are put there by `PlaybackService.resolveVideo`, so no extra
		 * `getVideoInfo` call is needed here.
		 */
		fun MediaItem.captionConfigs(): List<MediaItem.SubtitleConfiguration> =
			localConfiguration?.subtitleConfigurations.orEmpty()

		val COMMANDS: Player.Commands = Player.Commands.Builder()
			.addAll(
				Player.COMMAND_PLAY_PAUSE,
				Player.COMMAND_PREPARE,
				Player.COMMAND_STOP,
				Player.COMMAND_SEEK_TO_DEFAULT_POSITION,
				Player.COMMAND_SEEK_IN_CURRENT_MEDIA_ITEM,
				Player.COMMAND_SEEK_TO_PREVIOUS_MEDIA_ITEM,
				Player.COMMAND_SEEK_TO_PREVIOUS,
				Player.COMMAND_SEEK_TO_NEXT_MEDIA_ITEM,
				Player.COMMAND_SEEK_TO_NEXT,
				Player.COMMAND_SEEK_TO_MEDIA_ITEM,
				Player.COMMAND_SET_MEDIA_ITEM,
				Player.COMMAND_CHANGE_MEDIA_ITEMS,
				Player.COMMAND_GET_CURRENT_MEDIA_ITEM,
				Player.COMMAND_GET_TIMELINE,
				Player.COMMAND_GET_METADATA,
				// Without this the MediaController answers Tracks.EMPTY
				// however well this player fills them in, and the subtitle
				// picker — which reads controller.currentTracks — vanishes the
				// moment playback goes remote.
				Player.COMMAND_GET_TRACKS,
				Player.COMMAND_RELEASE,
			)
			.build()
	}
}
