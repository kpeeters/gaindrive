package org.gaindrive.android.playback

import android.util.Log
import android.view.SurfaceView
import androidx.media3.common.C
import androidx.media3.common.Player
import androidx.media3.common.TrackSelectionOverride
import androidx.media3.common.Tracks
import androidx.media3.common.VideoSize
import androidx.media3.common.text.CueGroup
import androidx.media3.exoplayer.ExoPlayer
import androidx.media3.ui.SubtitleView
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.asStateFlow
import javax.inject.Inject
import javax.inject.Singleton

/**
 * Connects the video screen's output — picture and subtitles — to the player
 * producing them.
 *
 * Everything else in the UI reaches playback through [PlayerConnection]'s
 * `MediaController`, and that stays true: the screen calls
 * `PlayerConnection.attachVideo`, not this class. What is different is the last
 * hop, where the surface is handed to the [ExoPlayer] itself rather than sent
 * over the session.
 *
 * That is deliberate, and it covers three things at once. A `MediaController`
 * can carry a surface only if the session grants `COMMAND_SET_VIDEO_SURFACE`,
 * and cues and video-size events have to survive the session boundary as well —
 * three negotiations whose failure mode is a black rectangle with nothing in
 * the log. The service and the UI share a process (`PlaybackService` declares
 * no `android:process`), so the direct reference is available, exact, and has
 * no state to get wrong. The local player stays the right target even while the
 * session is driving a Chromecast: while casting, `VideoScreen` composes a panel
 * instead of the surface, so nothing attaches here at all and the question of
 * which player owns it does not arise.
 *
 * Both ends can arrive in either order — the screen can be composed before the
 * service exists, and the service can be destroyed while the screen is still up
 * — so both halves are held and applied whenever both are present. Every method
 * runs on the main thread; the service's `onCreate` and Compose's effects both
 * do.
 */
@Singleton
class VideoSurface @Inject constructor() {

	private var player: ExoPlayer? = null
	private var surface: SurfaceView? = null
	private var subtitles: SubtitleView? = null

	private val _aspectRatio = MutableStateFlow<Float?>(null)

	/**
	 * The picture's shape once the decoder has reported it. Null until then, at
	 * which point the screen falls back to the figure the server gave — which
	 * is what stops the surface starting square and snapping.
	 */
	val aspectRatio: StateFlow<Float?> = _aspectRatio.asStateFlow()

	private val listener = object : Player.Listener {
		override fun onVideoSizeChanged(videoSize: VideoSize) {
			val width = videoSize.width * videoSize.pixelWidthHeightRatio
			_aspectRatio.value =
				if (width > 0f && videoSize.height > 0) width / videoSize.height else null
		}

		override fun onCues(cueGroup: CueGroup) {
			subtitles?.setCues(cueGroup.cues)
		}

		/**
		 * Every text track the player resolved, including the ones
		 * [subtitleGroups] declines to offer — which is the whole reason to log
		 * it. A side-loaded WebVTT, a track inside the container and a caption
		 * stream ExoPlayer invented for an HLS playlist are indistinguishable
		 * in the picker and differ only by MIME type, so a report of "the
		 * subtitles do not show" is answerable from this line and from almost
		 * nothing else.
		 */
		override fun onTracksChanged(tracks: Tracks) {
			tracks.groups.filter { it.type == C.TRACK_TYPE_TEXT }
				.forEachIndexed { i, group ->
					val format = group.mediaTrackGroup.getFormat(0)
					Log.d(TAG, "text track $i: mime=${format.sampleMimeType}" +
						" label=${format.label} lang=${format.language}" +
						" supported=${group.isTrackSupported(0)}" +
						" selected=${group.isSelected}")
				}
			pinFirstAudioTrack(tracks)
		}
	}

	/** Called by [PlaybackService] with its player, and with null on destroy. */
	fun registerPlayer(player: ExoPlayer?) {
		this.player?.removeListener(listener)
		this.player = player
		player?.addListener(listener)
		apply()
	}

	fun attach(surface: SurfaceView, subtitles: SubtitleView) {
		this.surface = surface
		this.subtitles = subtitles
		apply()
	}

	/**
	 * Takes [surface] back off the player, if it is still the one attached.
	 *
	 * The identity check matters when one video screen replaces another: the
	 * incoming screen attaches before the outgoing one disposes, and an
	 * unconditional clear would blank the surface that just arrived.
	 */
	fun detach(surface: SurfaceView) {
		if (this.surface !== surface) return
		this.surface = null
		this.subtitles = null
		player?.clearVideoSurface()
		// Not the decoder's business any more. Left set, the next video would
		// open showing the last line of the previous one.
		_aspectRatio.value = null
	}

	/**
	 * Turns on the subtitle track at [index] in the player's [subtitleGroups],
	 * or turns subtitles off when it is null. The numbering is the one
	 * [PlayerConnection] publishes, and both ends go through that one function
	 * so it cannot mean two different things.
	 *
	 * Applied to the player for the same reason the surface is: a controller
	 * can only carry this if the session grants
	 * `COMMAND_SET_TRACK_SELECTION_PARAMETERS`, and a refused command is a
	 * silent no-op — the user would tap a subtitle track and see nothing happen.
	 *
	 * **An index rather than the `TrackGroup` itself, and that is the whole
	 * fix.** `DefaultTrackSelector` looks an override up in a `HashMap` keyed on
	 * `TrackGroup`, whose equality compares the group's id and every field of
	 * every `Format`. A group read from the `MediaController` is rebuilt from
	 * the `PlayerInfo` bundle, so anything that does not survive that round trip
	 * makes the lookup miss — and a missed override throws nothing, logs
	 * nothing, and selects nothing. Resolving here, against the player's own
	 * `currentTracks`, makes the key the very object the selector will compare
	 * against. The index is what is safe to carry across the session boundary;
	 * the group is not.
	 *
	 * A stale index leaves the selection alone rather than falling through to
	 * "off": the tracks can republish between the menu opening and the tap, and
	 * turning the user's "turn on" into "turn off" would then persist across
	 * items.
	 *
	 * Disabling the whole track type is what "off" has to mean. Clearing the
	 * override alone hands the choice back to the default selector, which would
	 * promptly turn a track back on.
	 */
	fun selectTextTrack(index: Int?) {
		val player = player ?: return
		val builder = player.trackSelectionParameters.buildUpon()
			.clearOverridesOfType(C.TRACK_TYPE_TEXT)

		if (index == null) {
			player.trackSelectionParameters =
				builder.setTrackTypeDisabled(C.TRACK_TYPE_TEXT, true).build()
			return
		}

		val group = player.currentTracks.subtitleGroups().getOrNull(index)
		if (group == null) {
			Log.w(TAG, "subtitle track $index is gone; selection left alone")
			return
		}

		Log.d(TAG, "selecting text track $index:" +
			" mime=${group.mediaTrackGroup.getFormat(0).sampleMimeType}" +
			" supported=${group.isTrackSupported(0)}")
		player.trackSelectionParameters = builder
			.setTrackTypeDisabled(C.TRACK_TYPE_TEXT, false)
			.setOverrideForType(TrackSelectionOverride(group.mediaTrackGroup, 0))
			.build()
	}

	/**
	 * Keeps the film's *first* audio track, which is the one the server used to
	 * pick on our behalf.
	 *
	 * A remux or a re-encode passes `-map 0:a:0`, and that is not arbitrary:
	 * ffmpeg's own "best stream" rule scores by channel count and would take a
	 * 5.1 director's commentary over the stereo mix. Since the server started
	 * handing over whole containers (see `PlayableContainers`), the same choice
	 * falls to `DefaultTrackSelector` — which scores by preferred language, then
	 * by channel count, so it can reach the same wrong answer and there is no
	 * picker to undo it with.
	 *
	 * Guarded on there being more than one group rather than applied always, so
	 * the single-track case — every remux, every HLS stream — keeps whatever the
	 * selector would have done and this cannot regress a path it has no business
	 * touching.
	 *
	 * An override rather than a preferred language because there is nothing to
	 * prefer: the honest statement is "the first one", and it is what playback
	 * did before. An audio-track picker mirroring [selectTextTrack] is the real
	 * answer and is deliberately not attempted here.
	 */
	private fun pinFirstAudioTrack(tracks: Tracks) {
		val player = player ?: return
		val audio = tracks.groups.filter { it.type == C.TRACK_TYPE_AUDIO }
		if (audio.size < 2) return
		val first = audio.first().mediaTrackGroup
		// Nothing to do if it is already the selection — onTracksChanged fires
		// again for our own override, and re-applying it would loop.
		if (audio.first().isSelected) return
		Log.d(TAG, "pinning the first of ${audio.size} audio tracks:" +
			" lang=${first.getFormat(0).language}")
		player.trackSelectionParameters = player.trackSelectionParameters.buildUpon()
			.clearOverridesOfType(C.TRACK_TYPE_AUDIO)
			.setOverrideForType(TrackSelectionOverride(first, 0))
			.build()
	}

	private fun apply() {
		val player = player ?: return
		val surface = surface ?: return
		player.setVideoSurfaceView(surface)
	}

	private companion object {
		const val TAG = "GainDriveVideo"
	}
}
