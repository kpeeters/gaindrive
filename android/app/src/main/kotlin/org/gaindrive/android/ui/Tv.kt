package org.gaindrive.android.ui

import android.app.UiModeManager
import android.content.Context
import android.content.pm.PackageManager
import android.content.res.Configuration
import androidx.compose.foundation.IndicationNodeFactory
import androidx.compose.foundation.LocalIndication
import androidx.compose.foundation.interaction.FocusInteraction
import androidx.compose.foundation.interaction.InteractionSource
import androidx.compose.material3.ripple
import androidx.compose.runtime.Composable
import androidx.compose.runtime.CompositionLocalProvider
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.runtime.staticCompositionLocalOf
import androidx.compose.ui.Modifier
import androidx.compose.ui.composed
import androidx.compose.ui.draw.drawWithContent
import androidx.compose.ui.focus.onFocusChanged
import androidx.compose.ui.geometry.CornerRadius
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.graphics.drawscope.ContentDrawScope
import androidx.compose.ui.graphics.drawscope.DrawScope
import androidx.compose.ui.graphics.drawscope.Stroke
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
@Composable
fun TvEnvironment(isTv: Boolean, content: @Composable () -> Unit) {
	if (!isTv) {
		CompositionLocalProvider(LocalIsTv provides false, content = content)
	} else {
		CompositionLocalProvider(
			LocalIsTv provides true,
			LocalIndication provides remember { TvIndication(ripple()) },
			content = content,
		)
	}
}

/**
 * A focus ring for the components whose ripple is built in rather than taken
 * from [LocalIndication] (IconButton, Slider), which the [TvIndication]
 * override therefore cannot reach. A no-op off TV.
 */
fun Modifier.tvFocusHighlight(): Modifier = composed {
	if (!LocalIsTv.current) return@composed Modifier
	var focused by remember { mutableStateOf(false) }
	Modifier
		.onFocusChanged { focused = it.isFocused }
		.drawWithContent {
			drawContent()
			if (focused) drawFocusRing()
		}
}

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

	override fun onAttach() {
		coroutineScope.launch {
			interactionSource.interactions.collect {
				when (it) {
					is FocusInteraction.Focus -> {
						focused = true
						invalidateDraw()
					}
					is FocusInteraction.Unfocus -> {
						focused = false
						invalidateDraw()
					}
				}
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
		if (focused) drawFocusRing()
	}
}

/**
 * Sand over the brand red, hardcoded because a Modifier.Node has no theme
 * access: visible on both color schemes, on artwork, and on video chrome.
 */
private fun DrawScope.drawFocusRing() {
	val corner = CornerRadius(8.dp.toPx())
	drawRoundRect(
		color = Color.White.copy(alpha = 0.10f),
		cornerRadius = corner,
	)
	drawRoundRect(
		color = Color(0xFFF4D58D),
		style = Stroke(width = 3.dp.toPx()),
		cornerRadius = corner,
	)
}
