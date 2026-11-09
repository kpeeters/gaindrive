package org.gaindrive.android.ui

import android.app.UiModeManager
import android.content.Context
import android.content.pm.PackageManager
import android.content.res.Configuration
import android.util.Log
import androidx.compose.animation.core.Animatable
import androidx.compose.animation.core.tween
import androidx.compose.foundation.ExperimentalFoundationApi
import androidx.compose.foundation.IndicationNodeFactory
import androidx.compose.foundation.LocalIndication
import androidx.compose.foundation.gestures.BringIntoViewSpec
import androidx.compose.foundation.gestures.LocalBringIntoViewSpec
import androidx.compose.foundation.interaction.FocusInteraction
import androidx.compose.foundation.interaction.InteractionSource
import androidx.compose.material.ripple.RippleAlpha
import androidx.compose.material3.ExperimentalMaterial3Api
import androidx.compose.material3.LocalRippleConfiguration
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.RippleConfiguration
import androidx.compose.material3.TopAppBarColors
import androidx.compose.material3.TopAppBarDefaults
import androidx.compose.material3.ripple
import androidx.compose.runtime.Composable
import androidx.compose.runtime.CompositionLocalProvider
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.compositionLocalOf
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.runtime.staticCompositionLocalOf
import androidx.compose.runtime.withFrameNanos
import androidx.compose.ui.Modifier
import androidx.compose.ui.composed
import androidx.compose.ui.draw.drawWithContent
import androidx.compose.ui.focus.FocusDirection
import androidx.compose.ui.focus.FocusRequester
import androidx.compose.ui.focus.focusRequester
import androidx.compose.ui.focus.onFocusChanged
import androidx.compose.ui.geometry.CornerRadius
import androidx.compose.ui.geometry.Offset
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.graphics.drawscope.ContentDrawScope
import androidx.compose.ui.graphics.graphicsLayer
import androidx.compose.ui.graphics.lerp
import androidx.compose.ui.node.DelegatableNode
import androidx.compose.ui.node.DelegatingNode
import androidx.compose.ui.node.DrawModifierNode
import androidx.compose.ui.node.invalidateDraw
import androidx.compose.ui.unit.dp
import kotlin.math.abs
import kotlinx.coroutines.flow.filter
import kotlinx.coroutines.launch

/**
 * TV (d-pad) support. The one place that knows the app can run on a
 * television: detection, and the focus ring without which a remote user
 * cannot see where they are. Everything is inert off TV, so phone, tablet
 * and Chromebook rendering is untouched.
 */

/** True on Android TV and Google TV devices. Never changes mid-process. */
val LocalIsTv = staticCompositionLocalOf { false }

fun isTvDevice(context: Context): Boolean {
	// uiMode is the authoritative signal on Google TV; the leanback feature
	// catches boxes whose uiMode reports oddly. Either alone has exceptions.
	val uiMode = context.getSystemService(UiModeManager::class.java)
	return uiMode?.currentModeType == Configuration.UI_MODE_TYPE_TELEVISION ||
		context.packageManager.hasSystemFeature(PackageManager.FEATURE_LEANBACK)
}

/**
 * Wraps [content] in the TV environment: [LocalIsTv], and on a TV the focus
 * shown as an accent tint, the web client's tinted pill.
 *
 * The tint is M3's own focus state layer, recoloured through
 * [LocalRippleConfiguration]. Every M3 component clips that layer to its own
 * shape, so a pill gets a pill and an icon a circle; a ring drawn by us saw
 * only the 48dp touch target and stuck out of panes. Rows are bare clickables
 * resolving [LocalIndication], which wraps the same ripple and adds a pulse.
 */
@OptIn(ExperimentalMaterial3Api::class)
@Composable
fun TvEnvironment(isTv: Boolean, content: @Composable () -> Unit) {
	if (!isTv) {
		CompositionLocalProvider(LocalIsTv provides false, content = content)
	} else {
		val accent = MaterialTheme.colorScheme.primary
		val ripple = remember(accent) {
			RippleConfiguration(
				color = accent,
				rippleAlpha = RippleAlpha(
					draggedAlpha = 0.16f,
					focusedAlpha = 0.24f,
					hoveredAlpha = 0.08f,
					pressedAlpha = 0.12f,
				),
			)
		}
		CompositionLocalProvider(
			LocalIsTv provides true,
			LocalIndication provides remember(accent) { TvIndication(ripple(), accent) },
			LocalRippleConfiguration provides ripple,
			content = content,
		)
	}
}

/**
 * The arrival pulse for the components whose ripple is built in rather than
 * taken from [LocalIndication] (Button, IconButton), which the row pulse in
 * [TvIndication] therefore cannot reach: a brief bump in size that settles
 * back, so nothing stays outside its slot. Their tint needs nothing here.
 * [grow] is false for a full-width control (a seek bar), which would push
 * past the screen edge. A no-op off TV.
 */
fun Modifier.tvFocusHighlight(grow: Boolean = true): Modifier = composed {
	if (!LocalIsTv.current || !grow) return@composed Modifier
	var focused by remember { mutableStateOf(false) }
	val scale = remember { Animatable(1f) }
	LaunchedEffect(focused) {
		if (focused) {
			scale.animateTo(BUMP_SCALE, tween(BUMP_UP_MS))
			scale.animateTo(1f, tween(BUMP_DOWN_MS))
		} else {
			scale.snapTo(1f)
		}
	}
	Modifier
		.onFocusChanged { focused = it.isFocused }
		.graphicsLayer {
			scaleX = scale.value
			scaleY = scale.value
		}
}

/**
 * Whether the pane being composed holds the d-pad focus. Only ever true on
 * a TV, where three panes side by side otherwise give no hint which one the
 * next press will move in.
 */
val LocalPaneFocused = compositionLocalOf { false }

/** A pane's app bar colours: the title in the accent while its pane holds the focus. */
@OptIn(ExperimentalMaterial3Api::class)
@Composable
fun paneTopBarColors(): TopAppBarColors =
	if (!LocalPaneFocused.current) TopAppBarDefaults.topAppBarColors()
	else TopAppBarDefaults.topAppBarColors(titleContentColor = MaterialTheme.colorScheme.primary)

/** For a pane's app bar: an underline in the accent while its pane holds the focus. */
fun Modifier.paneFocusAccent(): Modifier = composed {
	if (!LocalPaneFocused.current) return@composed Modifier
	val color = MaterialTheme.colorScheme.primary
	// Over the content, not behind it: the bar paints its own background.
	Modifier.drawWithContent {
		drawContent()
		val width = 2.dp.toPx()
		drawLine(
			color = color,
			start = Offset(0f, size.height - width / 2),
			end = Offset(size.width, size.height - width / 2),
			strokeWidth = width,
		)
	}
}

/**
 * A request that the pane showing [route] take the d-pad focus, made when the
 * user confirms a choice. Without it focus stays on the row just chosen, and
 * reaching what that row opened is a sideways press the user has to know to
 * make. One-shot: a pane rebuilt later, when the window slides back over it,
 * must not pull focus the user has since moved elsewhere.
 */
class FocusClaim(val route: Route) {
	internal var pending = true
}

/** The claim addressed to the pane being composed, or null. */
val LocalFocusClaim = compositionLocalOf<FocusClaim?> { null }

/**
 * Where a claimed pane's focus lands. On a list it enters at the first
 * visible row; on a row it lands on that row. A no-op off TV.
 */
fun Modifier.claimsFocus(): Modifier = composed {
	if (!LocalIsTv.current) return@composed Modifier
	val claim = LocalFocusClaim.current
	val requester = remember { FocusRequester() }
	LaunchedEffect(claim) {
		if (claim == null || !claim.pending) return@LaunchedEffect
		// Lazy rows are composed during layout, after this effect starts, so
		// the first attempts can find nothing to focus yet.
		repeat(CLAIM_FRAMES) {
			withFrameNanos {}
			if (requester.requestFocus(FocusDirection.Enter)) {
				claim.pending = false
				return@LaunchedEffect
			}
		}
		claim.pending = false
		Log.w(TAG, "focus claim for ${claim.route} found nothing to focus")
	}
	Modifier.focusRequester(requester)
}

/**
 * [content] with focus scrolling that moves a list only when the focused
 * item is not fully on screen, and then just far enough. A TV's default
 * scrolls every newly focused item towards a fixed line near the top, which
 * on a screen with a header (an album's cover) scrolls the header away even
 * when the item was already in view. A no-op off TV.
 */
@OptIn(ExperimentalFoundationApi::class)
@Composable
fun TvMinimalScroll(content: @Composable () -> Unit) {
	if (!LocalIsTv.current) {
		content()
		return
	}
	CompositionLocalProvider(
		LocalBringIntoViewSpec provides MinimalBringIntoViewSpec,
		content = content,
	)
}

@OptIn(ExperimentalFoundationApi::class)
private object MinimalBringIntoViewSpec : BringIntoViewSpec {
	override fun calculateScrollDistance(
		offset: Float,
		size: Float,
		containerSize: Float,
	): Float {
		val trailing = offset + size
		return when {
			offset >= 0f && trailing <= containerSize -> 0f
			// Larger than the viewport and already covering it: any move
			// would hide part of what is showing.
			offset < 0f && trailing > containerSize -> 0f
			abs(offset) < abs(trailing - containerSize) -> offset
			else -> trailing - containerSize
		}
	}
}

private const val CLAIM_FRAMES = 10

private const val BUMP_SCALE = 1.1f
private const val BUMP_UP_MS = 120
private const val BUMP_DOWN_MS = 220

// Starts above the steady row tint and fades into it.
private const val PULSE_ALPHA = 0.35f
private const val PULSE_MS = 400
private const val TAG = "GainDriveTv"

/**
 * The ripple, plus a steady light tint while focused and a flash of [accent]
 * as focus arrives: a still tint is easy to lose track of when the d-pad
 * moves it, a flash says where it went. The steady tint is our own rather
 * than the ripple's: the accent at ripple strength is a maroon that reads as
 * black on a TV's dark surface, where a row needs to look lifted. The ripple
 * is kept from seeing focus at all, so its square layer does not show at the
 * corners of our rounded one.
 */
private class TvIndication(
	private val ripple: IndicationNodeFactory,
	private val accent: Color,
) : IndicationNodeFactory {
	override fun create(interactionSource: InteractionSource): DelegatableNode =
		TvFocusNode(interactionSource, ripple.create(PressOnly(interactionSource)), accent)

	// Value semantics delegate to the ripple's, so recomposition sees the
	// same indication and does not detach nodes.
	override fun equals(other: Any?) =
		other is TvIndication && other.ripple == ripple && other.accent == accent

	override fun hashCode() = 31 * ripple.hashCode() + accent.hashCode()
}

/** [source] without its focus interactions. */
private class PressOnly(source: InteractionSource) : InteractionSource {
	override val interactions = source.interactions.filter { it !is FocusInteraction }
}

private class TvFocusNode(
	private val interactionSource: InteractionSource,
	rippleNode: DelegatableNode,
	private val accent: Color,
) : DelegatingNode(), DrawModifierNode {
	// Both this node and the ripple draw, so the framework leaves the
	// delegate's draw to us; the reference is what we forward through.
	private val rippleDraw = delegate(rippleNode) as? DrawModifierNode
	private val steady = lerp(accent, Color.White, 0.45f).copy(alpha = 0.28f)
	private var focused = false
	private val pulse = Animatable(0f)

	override fun onAttach() {
		coroutineScope.launch {
			interactionSource.interactions.collect {
				when (it) {
					is FocusInteraction.Focus -> setFocused(true)
					is FocusInteraction.Unfocus -> setFocused(false)
				}
			}
		}
	}

	private fun setFocused(value: Boolean) {
		focused = value
		invalidateDraw()
		coroutineScope.launch {
			if (value) {
				pulse.snapTo(PULSE_ALPHA)
				pulse.animateTo(0f, tween(PULSE_MS)) { invalidateDraw() }
			} else {
				pulse.snapTo(0f)
				invalidateDraw()
			}
		}
	}

	override fun ContentDrawScope.draw() {
		val ripple = rippleDraw
		if (ripple != null) {
			with(ripple) { this@draw.draw() }
		} else {
			drawContent()
		}
		// The corners of the open row's fill in Rows.kt, so the two line up.
		val corner = CornerRadius(8.dp.toPx())
		if (focused) drawRoundRect(steady, cornerRadius = corner)
		if (pulse.value > 0f) drawRoundRect(accent.copy(alpha = pulse.value), cornerRadius = corner)
	}
}
