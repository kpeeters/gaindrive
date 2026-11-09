package org.gaindrive.android.ui

import android.app.UiModeManager
import android.content.Context
import android.content.pm.PackageManager
import android.content.res.Configuration
import android.util.Log
import androidx.compose.animation.core.Animatable
import androidx.compose.animation.core.animateFloatAsState
import androidx.compose.foundation.IndicationNodeFactory
import androidx.compose.foundation.LocalIndication
import androidx.compose.foundation.interaction.FocusInteraction
import androidx.compose.foundation.interaction.InteractionSource
import androidx.compose.foundation.shape.RoundedCornerShape
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
import androidx.compose.ui.geometry.Offset
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.graphics.Shape
import androidx.compose.ui.graphics.drawOutline
import androidx.compose.ui.graphics.drawscope.ContentDrawScope
import androidx.compose.ui.graphics.drawscope.DrawScope
import androidx.compose.ui.graphics.drawscope.Stroke
import androidx.compose.ui.graphics.drawscope.scale
import androidx.compose.ui.graphics.graphicsLayer
import androidx.compose.ui.node.DelegatableNode
import androidx.compose.ui.node.DelegatingNode
import androidx.compose.ui.node.DrawModifierNode
import androidx.compose.ui.node.invalidateDraw
import androidx.compose.ui.unit.dp
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
 * Wraps [content] in the TV environment: [LocalIsTv], and on a TV a
 * [LocalIndication] that draws a focus ring over the ordinary ripple.
 *
 * The indication route is chosen over decorating call sites because every
 * browse row is a bare clickable that resolves LocalIndication: one override
 * here gives all of them a visible focus without touching any of them.
 */
@OptIn(ExperimentalMaterial3Api::class)
@Composable
fun TvEnvironment(isTv: Boolean, content: @Composable () -> Unit) {
	if (!isTv) {
		CompositionLocalProvider(LocalIsTv provides false, content = content)
	} else {
		CompositionLocalProvider(
			LocalIsTv provides true,
			LocalIndication provides remember { TvIndication(ripple()) },
			// The safety net for M3 components that carry their own ripple and
			// were not given a tvFocusHighlight(): their focus tint, nearly
			// invisible at the default alpha, becomes readable on a TV.
			LocalRippleConfiguration provides TvRippleConfiguration,
			content = content,
		)
	}
}

@OptIn(ExperimentalMaterial3Api::class)
private val TvRippleConfiguration = RippleConfiguration(
	rippleAlpha = RippleAlpha(
		draggedAlpha = 0.16f,
		focusedAlpha = 0.35f,
		hoveredAlpha = 0.08f,
		pressedAlpha = 0.12f,
	),
)

/**
 * A focus ring for the components whose ripple is built in rather than taken
 * from [LocalIndication] (Button, IconButton, Slider), which the [TvIndication]
 * override therefore cannot reach. [shape] should follow the component's own
 * outline: an 8dp rectangle around a round icon reads as a different control.
 * [grow] is for compact controls; a full-width one (a seek bar) would push
 * past the screen edge. A no-op off TV.
 */
fun Modifier.tvFocusHighlight(shape: Shape = RingShape, grow: Boolean = true): Modifier = composed {
	if (!LocalIsTv.current) return@composed Modifier
	var focused by remember { mutableStateOf(false) }
	val target = if (focused && grow) CONTROL_SCALE else 1f
	val scale by animateFloatAsState(target, label = "tvFocusScale")
	Modifier
		.onFocusChanged { focused = it.isFocused }
		.graphicsLayer {
			scaleX = scale
			scaleY = scale
		}
		.drawWithContent {
			drawContent()
			if (focused) drawFocusRing(shape)
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

private const val CLAIM_FRAMES = 10

// Small for rows: they span a whole pane, and a few percent of that width is
// already a visible jump. Controls are small, so they can grow more.
private const val ROW_SCALE = 1.02f
private const val CONTROL_SCALE = 1.08f

private val RingShape = RoundedCornerShape(8.dp)
private const val TAG = "GainDriveTv"

/**
 * The ripple, plus a ring while focused. The default ripple does mark focus,
 * but as a faint overlay that vanishes from across a room; the ring is the
 * 10-foot version of the same state.
 */
private class TvIndication(private val ripple: IndicationNodeFactory) : IndicationNodeFactory {
	override fun create(interactionSource: InteractionSource): DelegatableNode =
		TvFocusNode(interactionSource, ripple.create(interactionSource))

	// Value semantics delegate to the ripple's, so recomposition sees the
	// same indication and does not detach nodes.
	override fun equals(other: Any?) =
		other is TvIndication && other.ripple == ripple

	override fun hashCode() = ripple.hashCode()
}

private class TvFocusNode(
	private val interactionSource: InteractionSource,
	rippleNode: DelegatableNode,
) : DelegatingNode(), DrawModifierNode {
	// Both this node and the ripple draw, so the framework leaves the
	// delegate's draw to us; the reference is what we forward through.
	private val rippleDraw = delegate(rippleNode) as? DrawModifierNode
	private var focused = false
	private val zoom = Animatable(1f)

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
			zoom.animateTo(if (value) ROW_SCALE else 1f) { invalidateDraw() }
		}
	}

	// Scaled at draw time rather than in layout: the row keeps its slot, so
	// the list does not reflow under the moving focus.
	override fun ContentDrawScope.draw() {
		scale(zoom.value) {
			val ripple = rippleDraw
			if (ripple != null) {
				with(ripple) { this@draw.draw() }
			} else {
				this@draw.drawContent()
			}
			if (focused) drawFocusRing(RingShape)
		}
	}
}

/**
 * Sand over the brand red, hardcoded because a Modifier.Node has no theme
 * access: visible on both color schemes, on artwork, and on video chrome.
 */
private fun DrawScope.drawFocusRing(shape: Shape) {
	val outline = shape.createOutline(size, layoutDirection, this)
	drawOutline(outline, color = Color.White.copy(alpha = 0.10f))
	drawOutline(outline, color = Color(0xFFF4D58D), style = Stroke(width = 3.dp.toPx()))
}
