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

	/** Whether the last [Player.Listener.onCues] carried anything to draw. */
	private var hadCues = false

	private val listener = object : Player.Listener {
		override fun onVideoSizeChanged(videoSize: VideoSize) {
			val width = videoSize.width * videoSize.pixelWidthHeightRatio
			_aspectRatio.value =
				if (width > 0f && videoSize.height > 0) width / videoSize.height else null
		}

		/**
		 * Logged on the edges only — cues change every line of dialogue, and a
		 * line per line is a log nobody can read. The edges are what answer the
		 * question anyway: whether cues arrive at all, and whether there was a
		 * view of any size to put them in when they did.
		 */
		override fun onCues(cueGroup: CueGroup) {
			val view = subtitles
			if (cueGroup.cues.isNotEmpty() != hadCues) {
				hadCues = cueGroup.cues.isNotEmpty()
				val where = view?.let { "${it.width}x${it.height}" } ?: "no view"
				Log.d(TAG, "cues ${if (hadCues) "started" else "stopped"}," +
					" ${cueGroup.cues.size} in $where")
			}
			view?.setCues(cueGroup.cues)
		}

		/**
		 * The text tracks the player has resolved, which is the only place the
		 * side-loaded WebVTT and a subtitle track embedded in the container can
		 * be told apart — they look identical in the picker.
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
	 * Turns on the subtitle track at [index] among the player's text tracks, or
	 * turns subtitles off when it is null. The numbering is the one
	 * [PlayerConnection] publishes.
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

		val group = player.currentTracks.groups
			.filter { it.type == C.TRACK_TYPE_TEXT }
			.getOrNull(index)
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

	private fun apply() {
		val player = player ?: return
		val surface = surface ?: return
		player.setVideoSurfaceView(surface)
	}

	private companion object {
		const val TAG = "GainDriveVideo"
	}
}
