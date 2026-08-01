package org.gaindrive.android.playback.cast

import android.os.Looper
import androidx.media3.common.C
import androidx.media3.common.MediaItem
import androidx.media3.common.Player
import androidx.media3.common.SimpleBasePlayer
import com.google.common.util.concurrent.Futures
import com.google.common.util.concurrent.ListenableFuture
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.Job
import kotlinx.coroutines.launch
import org.gaindrive.android.data.StreamUrls
import org.gaindrive.android.playback.itemRef
import org.gaindrive.android.playback.sourceContentType

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
	private val streamUrls: StreamUrls,
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
			.build()
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
		mediaItems: List<MediaItemData>,
		startIndex: Int,
		startPositionMs: Long,
	): ListenableFuture<*> {
		entries = mediaItems.map { Entry(nextUid++, it.mediaItem) }
		index = if (startIndex == C.INDEX_UNSET) 0 else startIndex.coerceIn(0, maxOf(0, entries.size - 1))
		ended = false
		loadCurrent(if (startPositionMs == C.TIME_UNSET) 0 else startPositionMs)
		return Futures.immediateVoidFuture()
	}

	override fun handleAddMediaItems(
		index: Int,
		mediaItems: List<MediaItemData>,
	): ListenableFuture<*> {
		val added = mediaItems.map { Entry(nextUid++, it.mediaItem) }
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
	 * The URL comes from [StreamUrls] exactly as it does for local playback, so
	 * a cast track is the same quality, under the same per-server bitrate cap,
	 * as one played on the phone.
	 */
	private fun loadCurrent(positionMs: Long) {
		val entry = entries.getOrNull(index) ?: return
		val ref = entry.item.itemRef() ?: return
		ended = false
		loadJob?.cancel()
		loadJob = scope.launch {
			val target = streamUrls.forPlayback(ref) ?: return@launch
			val metadata = entry.item.mediaMetadata
			session.load(
				CastMedia(
					url = target.url,
					// Null for the original file, where the transcode's MIME does
					// not apply and the server's own content type is the answer.
					mimeType = target.mimeType ?: entry.item.sourceContentType(),
					durationSeconds = metadata.durationMs
						?.takeIf { it > 0 }
						?.let { it / 1000.0 },
					startSeconds = positionMs / 1000f,
					title = metadata.title?.toString(),
					artist = metadata.artist?.toString(),
					album = metadata.albumTitle?.toString(),
					artworkUrl = metadata.artworkUri?.toString(),
				)
			)
		}
	}

	private companion object {
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
				Player.COMMAND_RELEASE,
			)
			.build()
	}
}
