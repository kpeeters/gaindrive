package org.gaindrive.android.ui.player

import android.app.Activity
import android.content.ContextWrapper
import android.content.res.Configuration
import android.view.SurfaceView
import android.view.View
import android.view.ViewGroup
import android.view.Window
import android.widget.FrameLayout
import androidx.activity.compose.BackHandler
import androidx.compose.animation.AnimatedVisibility
import androidx.compose.animation.fadeIn
import androidx.compose.animation.fadeOut
import androidx.compose.animation.slideInHorizontally
import androidx.compose.animation.slideOutHorizontally
import androidx.compose.foundation.background
import androidx.compose.foundation.clickable
import androidx.compose.foundation.interaction.MutableInteractionSource
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.aspectRatio
import androidx.compose.foundation.layout.fillMaxHeight
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.layout.widthIn
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.automirrored.filled.ArrowBack
import androidx.compose.material.icons.automirrored.filled.Toc
import androidx.compose.material.icons.filled.Cast
import androidx.compose.material.icons.filled.CastConnected
import androidx.compose.material.icons.filled.ClosedCaption
import androidx.compose.material.icons.filled.Info
import androidx.compose.material.icons.filled.Pause
import androidx.compose.material.icons.filled.PlayArrow
import androidx.compose.material.icons.filled.SkipNext
import androidx.compose.material.icons.filled.SkipPrevious
import androidx.compose.material3.Button
import androidx.compose.material3.CircularProgressIndicator
import androidx.compose.material3.DropdownMenu
import androidx.compose.material3.DropdownMenuItem
import androidx.compose.material3.Icon
import androidx.compose.material3.IconButton
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Surface
import androidx.compose.material3.Text
import androidx.compose.material3.TextButton
import androidx.compose.runtime.Composable
import androidx.compose.runtime.DisposableEffect
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableIntStateOf
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.saveable.rememberSaveable
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.platform.LocalConfiguration
import androidx.compose.ui.platform.LocalView
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.dp
import androidx.compose.ui.viewinterop.AndroidView
import androidx.core.view.WindowCompat
import androidx.core.view.WindowInsetsCompat
import androidx.core.view.WindowInsetsControllerCompat
import androidx.hilt.navigation.compose.hiltViewModel
import androidx.lifecycle.compose.collectAsStateWithLifecycle
import androidx.media3.ui.SubtitleView
import kotlinx.coroutines.delay
import org.gaindrive.android.data.model.currentAt
import org.gaindrive.android.data.model.nextAfter
import org.gaindrive.android.data.model.previousTargetMs
import org.gaindrive.android.playback.PlayerState
import org.gaindrive.android.ui.components.CoverArt

/**
 * The picture, and controls over it.
 *
 * Deliberately not media3's `PlayerView`. Its controls are its own, not
 * Material 3, and they would be the one part of the app that does not look like
 * the rest; the pieces actually needed here — a surface, a subtitle view and a
 * scrub bar — are three composables and the [SeekBar] already existed.
 *
 * Backing out does not stop playback: the surface detaches, the sound carries
 * on, and the mini-player offers the way back in. That is the choice made for
 * concert recordings, which are listened to as often as they are watched.
 */
@Composable
fun VideoScreen(
	onBack: () -> Unit,
	onCast: () -> Unit,
	onInfo: () -> Unit,
	viewModel: PlayerViewModel = hiltViewModel(),
	castViewModel: CastViewModel = hiltViewModel(),
	chapterViewModel: VideoChaptersViewModel = hiltViewModel(),
) {
	val state by viewModel.state.collectAsStateWithLifecycle()
	val decodedAspect by viewModel.videoAspectRatio.collectAsStateWithLifecycle()
	val castDevice by castViewModel.connected.collectAsStateWithLifecycle()
	val chapterList by chapterViewModel.state.collectAsStateWithLifecycle()
	val current = state.current
	val error = state.error

	val chapters = chapterList.chapters
	val currentChapter = chapters.currentAt(state.positionMs)
	// Saveable because this screen is rotated constantly, and because the web
	// client's panel likewise survives the film advancing to the next item.
	var chaptersOpen by rememberSaveable { mutableStateOf(false) }
	// A recording that offers nothing must not leave the panel open behind it.
	LaunchedEffect(chapters.isEmpty()) {
		if (chapters.isEmpty()) chaptersOpen = false
	}

	var controlsVisible by remember { mutableStateOf(true) }
	// Bumped on every touch; the auto-hide effect restarts with it, so a tap
	// during the countdown extends the reprieve rather than being ignored.
	var interactionTick by remember { mutableIntStateOf(0) }

	// What a side swipe is adjusting, and the last thing it adjusted — the
	// second held separately so the indicator has something to draw through its
	// own fade-out. Deliberately *not* bumping `interactionTick`: nudging the
	// volume must not drag the whole transport back over the picture, which is
	// the opposite of what the gesture is for.
	var adjustment by remember { mutableStateOf<SideAdjustment?>(null) }
	var hud by remember { mutableStateOf<SideAdjustment?>(null) }
	var hudVisible by remember { mutableStateOf(false) }
	LaunchedEffect(adjustment) {
		adjustment?.let {
			hud = it
			hudVisible = true
			return@LaunchedEffect
		}
		delay(HUD_LINGER_MS)
		hudVisible = false
	}

	// Nothing to watch any more: the queue moved on to a track, or emptied.
	// Staying would leave a black rectangle with an inert seek bar under it.
	// Safe to fire on the first frame, because the only ways here are a video
	// having just become current and the Watch button on a video.
	//
	// The wait is what makes it safe to fire while casting. Connecting a device
	// swaps the session's player, and the new one publishes an empty queue for
	// the instant between being installed and being given the items — which
	// reads here as "no longer a video" and would drop the user off this screen
	// the moment they cast. A key change cancels the pending coroutine, so the
	// flicker back to true simply calls this off.
	LaunchedEffect(state.isVideo) {
		if (state.isVideo) return@LaunchedEffect
		delay(SWAP_GRACE_MS)
		onBack()
	}

	LaunchedEffect(interactionTick, state.isPlaying) {
		// Controls over a paused picture are not in the way of anything, and
		// hiding them would leave no visible way to resume.
		if (!state.isPlaying) return@LaunchedEffect
		delay(CONTROLS_TIMEOUT_MS)
		controlsVisible = false
	}

	// The screen must not sleep while a film is on: nothing is touching it, so
	// the system has no other reason to believe it is being watched. This is
	// the one thing PlayerView would have done for us. Not while casting — the
	// picture is on the television and there is nothing here to keep awake.
	val view = LocalView.current
	val keepAwake = castDevice == null
	DisposableEffect(view, keepAwake) {
		view.keepScreenOn = keepAwake
		onDispose { view.keepScreenOn = false }
	}

	// In landscape the picture is the whole screen, and the activity is
	// edge-to-edge, so the system bars sit *over* the film rather than beside
	// it — a navigation triplet down one corner of every shot. Hidden rather
	// than dimmed, and swipeable back transiently, which is what a video player
	// is expected to do.
	//
	// Portrait keeps its bars. The picture is letterboxed there with black to
	// spare, so they are over nothing, and bars that came and went with the
	// rotation would read as a glitch. Nor while casting: that is a panel about
	// a television, with nothing to be immersed in.
	val landscape =
		LocalConfiguration.current.orientation == Configuration.ORIENTATION_LANDSCAPE
	val immersive = landscape && castDevice == null
	DisposableEffect(view, immersive) {
		val bars = view.activityWindow()?.let { WindowCompat.getInsetsController(it, view) }
		if (immersive && bars != null) {
			bars.systemBarsBehavior =
				WindowInsetsControllerCompat.BEHAVIOR_SHOW_TRANSIENT_BARS_BY_SWIPE
			bars.hide(WindowInsetsCompat.Type.systemBars())
		}
		// Unconditional, and that is the point: leaving the screen, rotating
		// back and starting to cast all have to give the bars back. A screen
		// that swallowed them would leave no way out of itself.
		onDispose { bars?.show(WindowInsetsCompat.Type.systemBars()) }
	}

	// Back is the same gesture as the button, and both mean "leave the picture",
	// not "stop the film". The panel takes it first, though: a viewer who has
	// opened a jump list means to close that, not to walk out of the film.
	BackHandler {
		if (chaptersOpen) chaptersOpen = false else onBack()
	}

	Box(
		modifier = Modifier
			.fillMaxSize()
			.background(Color.Black)
			.clickable(
				interactionSource = remember { MutableInteractionSource() },
				// No ripple: this is the whole screen, and a ripple across the
				// picture reads as a glitch rather than a response.
				indication = null,
			) {
				controlsVisible = !controlsVisible
				interactionTick++
			}
			// After `clickable` and not before: `clickable` consumes the down
			// it is given, so from outside it this would be waiting for an
			// unconsumed one for ever. See videoSideGestures.
			.videoSideGestures(
				// Nothing to dim while the picture is on a television, and the
				// volume that matters there is not this device's. The chapter
				// panel is the other exclusion: it sits on the trailing edge,
				// exactly where the volume zone is, and its list and this
				// gesture would both be waiting for vertical slop on the same
				// drag.
				enabled = castDevice == null && !chaptersOpen,
				onAdjust = { adjustment = it },
			),
		contentAlignment = Alignment.Center,
	) {
		// Not merely hidden while casting: composing it is what attaches the
		// surface to the local player, and a surface attached to a paused
		// ExoPlayer would sit here showing the frame it stopped on.
		val device = castDevice
		if (device == null) {
			VideoOutput(
				// The decoder's figure once it has one, the server's until then.
				// Starting square and snapping is the thing this avoids.
				aspectRatio = decodedAspect ?: current?.aspectRatio ?: DEFAULT_ASPECT,
				onSurface = { surface, subtitles -> viewModel.attachVideo(surface, subtitles) },
				onRelease = { surface -> viewModel.detachVideo(surface) },
			)
		} else {
			CastingElsewhere(artworkUrl = current?.artworkUrl, deviceName = device.name)
		}

		// Over the picture rather than beside it: a video that has not started
		// is a black rectangle, and nothing else says the app is still working.
		//
		// The error takes precedence, because it is the answer to the question
		// the spinner was posing. Anything that stalls long enough for the
		// watchdog to give up ends here rather than spinning indefinitely.
		when {
			// Read once into a local: `state` comes from a delegate, so the
			// compiler will not smart-cast a property reached through it.
			error != null -> StalledNotice(
				message = error,
				onRetry = viewModel::retry,
				onDismiss = viewModel::clearError,
			)

			state.isBuffering -> CircularProgressIndicator(color = Color.White)
		}

		AnimatedVisibility(
			visible = controlsVisible,
			enter = fadeIn(),
			exit = fadeOut(),
			modifier = Modifier.fillMaxSize(),
		) {
			Controls(
				title = current?.title.orEmpty(),
				subtitle = listOf(current?.artist, current?.album)
					.filterNot { it.isNullOrBlank() }
					.joinToString(" · "),
				state = state,
				casting = castDevice != null,
				onBack = onBack,
				onTogglePlay = {
					viewModel.togglePlayPause()
					interactionTick++
				},
				onNext = viewModel::next,
				onPrevious = viewModel::previous,
				onSeek = {
					viewModel.seekTo(it)
					interactionTick++
				},
				onSelectTextTrack = viewModel::selectTextTrack,
				onCast = onCast,
				onInfo = onInfo,
				chapterName = chapters.getOrNull(currentChapter)?.displayName,
				hasChapters = chapters.isNotEmpty(),
				chaptersOpen = chaptersOpen,
				onToggleChapters = { chaptersOpen = !chaptersOpen },
			)
		}

		// Outside the controls' AnimatedVisibility for the reason the chapter
		// panel is outside it: a swipe must neither bring the transport back
		// nor have its own readout fade away with it.
		AnimatedVisibility(
			visible = hudVisible,
			enter = fadeIn(),
			exit = fadeOut(),
			modifier = Modifier.align(Alignment.Center),
		) {
			hud?.let { SideAdjustmentHud(it) }
		}

		// Last child, so it draws over the controls' scrim rather than under it,
		// and outside their AnimatedVisibility so it does not auto-hide with
		// them. See ChapterPanel.
		AnimatedVisibility(
			visible = chaptersOpen,
			enter = slideInHorizontally { it },
			exit = slideOutHorizontally { it },
			modifier = Modifier.align(Alignment.CenterEnd),
		) {
			ChapterPanel(
				chapters = chapters,
				source = chapterList.source,
				currentIndex = currentChapter,
				onSeek = viewModel::seekTo,
				// "Previous" restarts the marker being played unless we are
				// already at it, which is what every physical transport does and
				// what lets it be pressed twice to go back a song.
				onPrevious = { viewModel.seekTo(chapters.previousTargetMs(state.positionMs)) },
				onNext = {
					chapters.nextAfter(state.positionMs)?.let { viewModel.seekTo(it.startMs) }
				},
				onClose = { chaptersOpen = false },
				modifier = Modifier
					.fillMaxHeight()
					.widthIn(max = CHAPTER_PANEL_MAX_WIDTH),
			)
		}
	}
}

/**
 * What stands in for the picture while it is on a television.
 *
 * The screen is not left, and the controls above it are untouched: the scrub
 * bar, the transport and the way back all stay exactly where they were, because
 * from here casting is a change of screen and not a change of activity.
 */
@Composable
private fun CastingElsewhere(artworkUrl: String?, deviceName: String) {
	Column(
		horizontalAlignment = Alignment.CenterHorizontally,
		verticalArrangement = Arrangement.spacedBy(16.dp),
	) {
		CoverArt(
			url = artworkUrl,
			contentDescription = null,
			modifier = Modifier.size(160.dp),
			cornerRadius = 8.dp,
		)
		Row(
			horizontalArrangement = Arrangement.spacedBy(8.dp),
			verticalAlignment = Alignment.CenterVertically,
		) {
			Icon(
				imageVector = Icons.Default.CastConnected,
				contentDescription = null,
				tint = Color.White,
			)
			Text(
				text = "Playing on $deviceName",
				color = Color.White,
				style = MaterialTheme.typography.bodyLarge,
			)
		}
	}
}

/**
 * What replaces the spinner once the watchdog has given up.
 *
 * Over the picture rather than in place of it: the surface must not be torn
 * down, or a retry would restart the decoder's output from nothing.
 */
@Composable
private fun StalledNotice(message: String, onRetry: () -> Unit, onDismiss: () -> Unit) {
	Surface(
		color = Color.Black.copy(alpha = 0.8f),
		shape = MaterialTheme.shapes.medium,
	) {
		Column(
			modifier = Modifier.padding(24.dp),
			horizontalAlignment = Alignment.CenterHorizontally,
			verticalArrangement = Arrangement.spacedBy(8.dp),
		) {
			Text(
				text = message,
				style = MaterialTheme.typography.bodyMedium,
				color = Color.White,
			)
			Row(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
				TextButton(onClick = onDismiss) { Text("Dismiss") }
				// Picks up where it stopped: stop() keeps the queue and the
				// position, so there is nothing to rebuild.
				Button(onClick = onRetry) { Text("Retry") }
			}
		}
	}
}

/**
 * The surface and the subtitle view, held at the video's shape.
 *
 * Both are plain Android views inside one [AndroidView], because a
 * [SurfaceView] has to be a real view for the decoder to render into and
 * subtitle cues arrive as `Cue` objects that `SubtitleView` already knows how
 * to lay out. The factory runs once — recreating a `SurfaceView` tears down and
 * restarts the decoder's output, which shows as a black flash.
 */
@Composable
private fun VideoOutput(
	aspectRatio: Float,
	onSurface: (SurfaceView, SubtitleView) -> Unit,
	onRelease: (SurfaceView) -> Unit,
) {
	AndroidView(
		// **No `fillMaxWidth`.** With the width pinned, `aspectRatio` has only
		// the height left to solve for, and in landscape the height it arrives
		// at is taller than the screen: the picture is cropped top and bottom,
		// and the SubtitleView — which draws its cues near its own bottom edge
		// — puts them below the display entirely. Left free, the modifier tries
		// the width first and falls back to the height, which is letterbox in
		// portrait and pillarbox in landscape. The parent Box centres it.
		modifier = Modifier.aspectRatio(aspectRatio),
		factory = { context ->
			val surface = SurfaceView(context)
			val subtitles = SubtitleView(context)
			FrameLayout(context).apply {
				layoutParams = ViewGroup.LayoutParams(
					ViewGroup.LayoutParams.MATCH_PARENT,
					ViewGroup.LayoutParams.MATCH_PARENT,
				)
				addView(surface)
				addView(subtitles)
				// Tagged so onRelease can find the surface again without this
				// composable holding a reference across recomposition.
				tag = surface
				onSurface(surface, subtitles)
			}
		},
		onRelease = { container -> (container.tag as? SurfaceView)?.let(onRelease) },
	)
}

@Composable
private fun Controls(
	title: String,
	subtitle: String,
	state: PlayerState,
	casting: Boolean,
	onBack: () -> Unit,
	onTogglePlay: () -> Unit,
	onNext: () -> Unit,
	onPrevious: () -> Unit,
	onSeek: (Long) -> Unit,
	onSelectTextTrack: (Int?) -> Unit,
	onCast: () -> Unit,
	onInfo: () -> Unit,
	chapterName: String?,
	hasChapters: Boolean,
	chaptersOpen: Boolean,
	onToggleChapters: () -> Unit,
) {
	Box(
		modifier = Modifier
			.fillMaxSize()
			// Scrim so white controls stay readable over a bright frame.
			.background(Color.Black.copy(alpha = CONTROLS_SCRIM)),
	) {
		Row(
			modifier = Modifier
				.align(Alignment.TopStart)
				.fillMaxWidth()
				.padding(horizontal = 4.dp),
			verticalAlignment = Alignment.CenterVertically,
		) {
			IconButton(onClick = onBack) {
				Icon(
					Icons.AutoMirrored.Filled.ArrowBack,
					contentDescription = "Back",
					tint = Color.White,
				)
			}
			Column(modifier = Modifier.weight(1f)) {
				Text(
					text = title,
					style = MaterialTheme.typography.titleMedium,
					color = Color.White,
					maxLines = 1,
					overflow = TextOverflow.Ellipsis,
				)
				if (subtitle.isNotBlank()) {
					Text(
						text = subtitle,
						style = MaterialTheme.typography.bodySmall,
						color = Color.White.copy(alpha = 0.7f),
						maxLines = 1,
						overflow = TextOverflow.Ellipsis,
					)
				}
				// Which song inside the recording is playing — the web client's
				// video bar says the same thing. A line of its own rather than
				// folded into the subtitle, so nothing is lost on a recording
				// whose markers happen to be unnamed, and tinted because it
				// says where you are rather than what this is.
				chapterName?.let {
					Text(
						text = it,
						style = MaterialTheme.typography.bodySmall,
						color = MaterialTheme.colorScheme.primary,
						maxLines = 1,
						overflow = TextOverflow.Ellipsis,
					)
				}
			}
			// Absent rather than disabled when nothing has markers, which is
			// almost every film — the same rule the subtitle picker below
			// follows, and for the same reason.
			if (hasChapters) {
				IconButton(onClick = onToggleChapters) {
					Icon(
						imageVector = Icons.AutoMirrored.Filled.Toc,
						contentDescription = "Chapters",
						tint = if (chaptersOpen) {
							MaterialTheme.colorScheme.primary
						} else {
							Color.White
						},
					)
				}
			}
			// Absent rather than disabled when there is nothing to choose: a
			// DVD's subtitles are bitmaps the server cannot convert, so an
			// inert button would be a permanent fixture on every disc rip.
			if (state.textTracks.isNotEmpty()) {
				SubtitleMenu(state, onSelectTextTrack)
			}
			// Here for the same reason the cast button is: a video takes the
			// app straight to this screen, so a control that lives only in the
			// Now Playing sheet is one a film never reaches. It answers most
			// for a video, which is where remuxing and the cast route are
			// hardest to guess at.
			IconButton(onClick = onInfo) {
				Icon(Icons.Default.Info, contentDescription = "Track info", tint = Color.White)
			}
			// The only way to reach a Chromecast from here. The Now Playing
			// sheet has the other one, and this screen is not reached through
			// it — a video takes the app straight here — so without this the
			// button exists in a place a film never visits.
			//
			// Offered only for a video the receiver could play, on the same
			// rule the sheet uses, and always while casting so the way back off
			// the television stays where it was.
			if (state.nativeSeek || casting) {
				IconButton(onClick = onCast) {
					Icon(
						imageVector = if (casting) {
							Icons.Default.CastConnected
						} else {
							Icons.Default.Cast
						},
						contentDescription = if (casting) "Casting" else "Cast",
						tint = Color.White,
					)
				}
			}
		}

		Column(
			modifier = Modifier
				.align(Alignment.BottomCenter)
				.fillMaxWidth(),
		) {
			Row(
				modifier = Modifier.fillMaxWidth(),
				horizontalArrangement = Arrangement.Center,
				verticalAlignment = Alignment.CenterVertically,
			) {
				IconButton(onClick = onPrevious, enabled = state.hasPrevious) {
					Icon(
						Icons.Default.SkipPrevious,
						contentDescription = "Previous",
						tint = Color.White,
						modifier = Modifier.size(36.dp),
					)
				}
				IconButton(onClick = onTogglePlay) {
					Icon(
						imageVector = if (state.isPlaying) {
							Icons.Default.Pause
						} else {
							Icons.Default.PlayArrow
						},
						contentDescription = if (state.isPlaying) "Pause" else "Play",
						tint = Color.White,
						modifier = Modifier.size(56.dp),
					)
				}
				IconButton(onClick = onNext, enabled = state.hasNext) {
					Icon(
						Icons.Default.SkipNext,
						contentDescription = "Next",
						tint = Color.White,
						modifier = Modifier.size(36.dp),
					)
				}
			}
			SeekBar(
				state = state,
				onSeek = onSeek,
				modifier = Modifier.padding(horizontal = 16.dp, vertical = 8.dp),
				textColor = Color.White,
			)
		}
	}
}

@Composable
private fun SubtitleMenu(
	state: PlayerState,
	onSelect: (Int?) -> Unit,
) {
	var open by remember { mutableStateOf(false) }
	Box {
		IconButton(onClick = { open = true }) {
			Icon(
				Icons.Default.ClosedCaption,
				contentDescription = "Subtitles",
				tint = if (state.textTracks.any { it.selected }) {
					MaterialTheme.colorScheme.primary
				} else {
					Color.White
				},
			)
		}
		DropdownMenu(expanded = open, onDismissRequest = { open = false }) {
			DropdownMenuItem(
				text = { Text("Off") },
				onClick = {
					onSelect(null)
					open = false
				},
			)
			state.textTracks.forEach { track ->
				DropdownMenuItem(
					text = {
						Text(
							text = track.label,
							color = if (track.selected) {
								MaterialTheme.colorScheme.primary
							} else {
								MaterialTheme.colorScheme.onSurface
							},
						)
					},
					onClick = {
						onSelect(track.index)
						open = false
					},
				)
			}
		}
	}
}

/** 16:9, until either the server or the decoder says otherwise. */
private const val DEFAULT_ASPECT = 16f / 9f

/** Long enough to read the title, short enough to get out of the way. */
private const val CONTROLS_TIMEOUT_MS = 3_500L

/**
 * The window this view belongs to, or null if there is somehow no Activity
 * above it.
 *
 * Compose hands out a themed `ContextWrapper` rather than the Activity itself,
 * so the chain has to be walked. `LocalActivity` would say this in one line,
 * but it arrived in activity-compose 1.10 and this app is on 1.9.
 *
 * Shared with `VideoGestures.kt`, which needs the same window to set a
 * brightness on, hence `internal` rather than private to this file.
 */
internal fun View.activityWindow(): Window? =
	generateSequence(context) { (it as? ContextWrapper)?.baseContext }
		.filterIsInstance<Activity>()
		.firstOrNull()
		?.window

/**
 * Long enough to cover the player swap that starts or ends a cast session,
 * short enough that a queue which really did move on does not sit here.
 */
private const val SWAP_GRACE_MS = 500L

private const val CONTROLS_SCRIM = 0.4f

/**
 * How long the brightness/volume readout stays after the finger lifts. Long
 * enough to see where it ended up, short enough not to become furniture — it
 * sits over the film, unlike the controls, which dim it first.
 */
private const val HUD_LINGER_MS = 600L

/**
 * Wide enough for a marker title, narrow enough to leave the picture readable.
 * One rule for both orientations: in portrait the film is letterboxed with room
 * to spare, so the panel covers black rather than picture.
 */
private val CHAPTER_PANEL_MAX_WIDTH = 320.dp
