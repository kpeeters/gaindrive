package org.gaindrive.android.ui.player

import android.content.Context
import android.media.AudioManager
import android.provider.Settings
import android.util.Log
import android.view.Window
import android.view.WindowManager
import androidx.compose.foundation.gestures.awaitEachGesture
import androidx.compose.foundation.gestures.awaitFirstDown
import androidx.compose.foundation.gestures.awaitVerticalTouchSlopOrCancellation
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.width
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.automirrored.filled.VolumeOff
import androidx.compose.material.icons.automirrored.filled.VolumeUp
import androidx.compose.material.icons.filled.BrightnessHigh
import androidx.compose.material.icons.filled.BrightnessLow
import androidx.compose.material3.Icon
import androidx.compose.material3.LinearProgressIndicator
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Surface
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.DisposableEffect
import androidx.compose.runtime.remember
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.graphics.vector.ImageVector
import androidx.compose.ui.input.pointer.pointerInput
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.platform.LocalView
import androidx.compose.ui.unit.dp
import kotlin.math.roundToInt

/** What a side swipe is currently doing, for [SideAdjustmentHud] to draw. */
data class SideAdjustment(val control: SideControl, val fraction: Float)

/**
 * Brightness and volume by dragging a finger up or down one side of the
 * picture, the way every player offering this does it: the left side dims and
 * brightens, the right is the volume.
 *
 * It exists because the two controls most wanted during a film are the two
 * hardest to reach - on a tablet the volume rocker is small and awkwardly
 * placed, and brightness is behind a swipe down into Quick Settings, which
 * takes the viewer out of the picture altogether.
 *
 * media3 offers nothing here. `PlayerView` handles taps for controller
 * visibility and can be given a double-tap listener, but no version of it has a
 * brightness or volume gesture; every app that has one wrote it. That costs
 * nothing, since this player is deliberately not `PlayerView` in the first
 * place - see [VideoScreen].
 *
 * What a swipe is worth is [sideControlAt] and [travelFraction], which are pure
 * arithmetic and tested as such. This is the part that cannot be: a pointer
 * stream, a window attribute and an `AudioManager`.
 *
 * It reports what it is doing through [onAdjust] - and `null` when the gesture
 * ends, which is what starts the indicator's countdown.
 *
 * **Attach it after `clickable`, not before**, which is not a preference.
 * Later in a chain is the inner pointer node, and it sees the main pass first -
 * while `clickable` consumes the *down* the moment it is handed one, so from
 * outside it this would be waiting for an unconsumed down that never arrives,
 * and would do nothing whatsoever. From inside, the tap cancels itself instead:
 * a consumed change is what `waitForUpOrCancellation` gives up on. And a
 * gesture that never reaches touch slop consumes nothing, so a tap inside a
 * zone is still an ordinary tap.
 *
 * [enabled] is honoured inside the pointer block rather than by returning early
 * from the composable, so no effect below is conditional on it.
 */
@Composable
fun Modifier.videoSideGestures(
	enabled: Boolean,
	onAdjust: (SideAdjustment?) -> Unit,
): Modifier {
	val view = LocalView.current
	val context = LocalContext.current
	val audio = remember(context) {
		context.getSystemService(Context.AUDIO_SERVICE) as? AudioManager
	}

	// A window attribute outlives the composable that set it, so leaving the
	// film has to hand the brightness back - which is what the override value
	// meaning "none" does, restoring adaptive brightness along with it.
	// Unconditional for the reason the system bars are restored
	// unconditionally: a screen that kept the display dimmed on its way out
	// would leave nothing saying why the rest of the app had gone dark.
	DisposableEffect(view) {
		onDispose { view.activityWindow()?.clearBrightnessOverride() }
	}

	return pointerInput(enabled, audio, view) {
		if (!enabled) return@pointerInput
		val inset = EDGE_INSET.toPx()
		awaitEachGesture {
			// requireUnconsumed, so a down already claimed by a control - the
			// back button, the transport, the captions menu - never starts one
			// of these. The same rule the tap handler relies on.
			val down = awaitFirstDown(requireUnconsumed = true)
			val control = sideControlAt(down.position.x, size.width.toFloat(), inset)
				?: return@awaitEachGesture
			// Resolved before slop is claimed rather than after, so a gesture
			// nothing can serve leaves the tap alone. Neither of these is ever
			// actually absent, which is the only reason it can be a return.
			val effector = when (control) {
				SideControl.Brightness -> view.activityWindow()?.let { window ->
					Effector(
						read = { window.brightnessLevel(context) },
						write = { window.setBrightnessLevel(it) },
					)
				}
				SideControl.Volume -> audio?.let { manager ->
					Effector(
						read = { manager.musicLevel() },
						write = { manager.setMusicLevel(it) },
					)
				}
			} ?: return@awaitEachGesture

			// Nothing is consumed until slop is reached, which is what leaves a
			// tap inside a zone to the tap handler. It is also what keeps a
			// horizontal scrub of the seek bar safe: the Slider claims that
			// drag, and a consumed change cancels this.
			val start = awaitVerticalTouchSlopOrCancellation(down.id) { change, _ ->
				change.consume()
			} ?: return@awaitEachGesture

			// Read at the start of every gesture rather than carried across
			// them, so a change made with the hardware keys in between is not
			// undone by the next swipe resuming where this one stopped.
			var level = effector.read()
			var lastY = start.position.y
			onAdjust(SideAdjustment(control, level))

			while (true) {
				val event = awaitPointerEvent()
				val change = event.changes.firstOrNull { it.id == down.id } ?: break
				if (!change.pressed) break
				val step = travelFraction(change.position.y - lastY, size.height.toFloat())
				level = (level + step).coerceIn(0f, 1f)
				lastY = change.position.y
				// Claimed, so the tap handler underneath does not also fire.
				change.consume()
				effector.write(level)
				onAdjust(SideAdjustment(control, level))
			}
			onAdjust(null)
		}
	}
}

/**
 * The transient indicator, in `StalledNotice`'s clothes - this screen draws in
 * literal white over a black scrim rather than from the colour scheme, because
 * everything here sits over a picture rather than a surface.
 */
@Composable
fun SideAdjustmentHud(adjustment: SideAdjustment, modifier: Modifier = Modifier) {
	Surface(
		modifier = modifier,
		color = Color.Black.copy(alpha = HUD_SCRIM),
		shape = MaterialTheme.shapes.medium,
	) {
		Column(
			modifier = Modifier.padding(20.dp),
			horizontalAlignment = Alignment.CenterHorizontally,
			verticalArrangement = Arrangement.spacedBy(10.dp),
		) {
			Icon(
				imageVector = adjustment.icon(),
				// Decorative: the figure below says the same thing, and a
				// gesture is not something a screen reader can reach anyway.
				// The accessible routes to these two are the hardware keys and
				// the system's own brightness control, both untouched.
				contentDescription = null,
				tint = Color.White,
			)
			LinearProgressIndicator(
				progress = { adjustment.fraction },
				modifier = Modifier.width(HUD_BAR_WIDTH),
				color = Color.White,
				trackColor = Color.White.copy(alpha = 0.3f),
			)
			Text(
				text = "${(adjustment.fraction * 100).roundToInt()}%",
				style = MaterialTheme.typography.labelLarge,
				color = Color.White,
			)
		}
	}
}

/**
 * One control's current value and how to change it, so the loop above is the
 * same loop whichever side the finger came down on.
 */
private class Effector(val read: () -> Float, val write: (Float) -> Unit)

private fun SideAdjustment.icon(): ImageVector = when {
	control == SideControl.Volume && fraction <= 0f -> Icons.AutoMirrored.Filled.VolumeOff
	control == SideControl.Volume -> Icons.AutoMirrored.Filled.VolumeUp
	fraction <= BRIGHTNESS_LOW_ICON -> Icons.Default.BrightnessLow
	else -> Icons.Default.BrightnessHigh
}

/**
 * Where a brightness gesture starts from.
 *
 * The window's own override once there is one. Before that there is not, and
 * the honest starting point is the system's setting - which is readable without
 * permission, unlike writing it. Its scale is *not* guaranteed to be 0..255:
 * the real maximum is a hidden `PowerManager` value, and 255 is only the usual
 * one. That approximation decides where the first drag of a session begins and
 * nothing else, and the drag itself corrects it.
 */
private fun Window.brightnessLevel(context: Context): Float {
	val override = attributes.screenBrightness
	if (override >= 0f) return override.coerceIn(0f, 1f)
	return runCatching {
		Settings.System.getInt(
			context.contentResolver,
			Settings.System.SCREEN_BRIGHTNESS,
		) / SYSTEM_BRIGHTNESS_SCALE
	}.getOrDefault(0.5f).coerceIn(0f, 1f)
}

/**
 * Floored rather than allowed to reach zero. Nothing here is a way *back* from
 * a screen too dark to read: the gesture that would undo it is invisible at
 * that point, and on some devices zero is very close to off.
 */
private fun Window.setBrightnessLevel(level: Float) {
	attributes = attributes.apply {
		screenBrightness = level.coerceIn(MIN_BRIGHTNESS, 1f)
	}
}

/** Hands the brightness back to the system, adaptive brightness with it. */
private fun Window.clearBrightnessOverride() {
	attributes = attributes.apply {
		screenBrightness = WindowManager.LayoutParams.BRIGHTNESS_OVERRIDE_NONE
	}
}

private fun AudioManager.musicLevel(): Float {
	val max = getStreamMaxVolume(AudioManager.STREAM_MUSIC)
	if (max <= 0) return 0f
	return (getStreamVolume(AudioManager.STREAM_MUSIC).toFloat() / max).coerceIn(0f, 1f)
}

/**
 * Writes the volume, if the rounded step has actually moved.
 *
 * The level is carried as a float and only quantised here, because the music
 * stream is typically fifteen steps and a swipe that moved in fifteen jumps
 * would feel like a ratchet rather than a slider.
 */
private fun AudioManager.setMusicLevel(level: Float) {
	val max = getStreamMaxVolume(AudioManager.STREAM_MUSIC)
	if (max <= 0) return
	val step = (level * max).roundToInt().coerceIn(0, max)
	if (step == getStreamVolume(AudioManager.STREAM_MUSIC)) return
	try {
		// Deliberately not FLAG_SHOW_UI: the system's own volume panel over the
		// film would be a second thing moving, fighting the indicator here.
		setStreamVolume(AudioManager.STREAM_MUSIC, step, 0)
	} catch (e: SecurityException) {
		// Do Not Disturb, with no notification-policy access. There is nothing
		// to be done about it from here, but a silent no-op reads as a gesture
		// that has broken.
		Log.w(TAG, "volume change refused", e)
	}
}

/**
 * How far in from each edge the zones start.
 *
 * Not at the edge, because from Android 10 that is the back-gesture region and
 * anything interactive there competes with the system for every drag beginning
 * near it. `AlphabetRail` records the same trap from the other direction.
 */
private val EDGE_INSET = 24.dp

/** The usual maximum of `Settings.System.SCREEN_BRIGHTNESS`. See above. */
private const val SYSTEM_BRIGHTNESS_SCALE = 255f

/** Dim, but never so dim that the way back cannot be found. */
private const val MIN_BRIGHTNESS = 0.02f

/** Below this the low-brightness glyph reads more honestly than the high one. */
private const val BRIGHTNESS_LOW_ICON = 0.4f

private const val HUD_SCRIM = 0.8f

/** Wide enough to read as a scale, narrow enough not to obscure the film. */
private val HUD_BAR_WIDTH = 120.dp

private const val TAG = "GainDriveVideo"
