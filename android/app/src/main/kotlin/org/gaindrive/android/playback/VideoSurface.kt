package org.gaindrive.android.playback

import android.view.SurfaceView
import androidx.media3.common.C
import androidx.media3.common.Player
import androidx.media3.common.TrackGroup
import androidx.media3.common.TrackSelectionOverride
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
 * no state to get wrong. Video is never cast either, so the local player is the
 * right target even while the session is driving a Chromecast.
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
	 * Turns on the subtitle track in [group], or turns subtitles off when it is
	 * null.
	 *
	 * Applied to the player for the same reason the surface is: a controller
	 * can only carry this if the session grants
	 * `COMMAND_SET_TRACK_SELECTION_PARAMETERS`, and a refused command is a
	 * silent no-op — the user would tap a subtitle track and see nothing happen.
	 *
	 * Disabling the whole track type is what "off" has to mean. Clearing the
	 * override alone hands the choice back to the default selector, which would
	 * promptly turn a track back on.
	 */
	fun selectTextTrack(group: TrackGroup?) {
		val player = player ?: return
		val builder = player.trackSelectionParameters.buildUpon()
			.clearOverridesOfType(C.TRACK_TYPE_TEXT)
		player.trackSelectionParameters = if (group == null) {
			builder.setTrackTypeDisabled(C.TRACK_TYPE_TEXT, true).build()
		} else {
			builder
				.setTrackTypeDisabled(C.TRACK_TYPE_TEXT, false)
				.setOverrideForType(TrackSelectionOverride(group, 0))
				.build()
		}
	}

	private fun apply() {
		val player = player ?: return
		val surface = surface ?: return
		player.setVideoSurfaceView(surface)
	}
}
