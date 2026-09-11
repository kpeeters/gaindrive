package org.gaindrive.android.ui.adaptive

import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.BoxWithConstraints
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.padding
import androidx.compose.material3.ExperimentalMaterial3Api
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Scaffold
import androidx.compose.material3.Text
import androidx.compose.material3.TopAppBar
import androidx.compose.material3.adaptive.ExperimentalMaterial3AdaptiveApi
import androidx.compose.material3.adaptive.currentWindowAdaptiveInfo
import androidx.compose.material3.adaptive.layout.AnimatedPane
import androidx.compose.material3.adaptive.layout.ListDetailPaneScaffold
import androidx.compose.material3.adaptive.layout.PaneAdaptedValue
import androidx.compose.material3.adaptive.layout.PaneScaffoldDirective
import androidx.compose.material3.adaptive.layout.ThreePaneScaffoldValue
import androidx.compose.material3.adaptive.layout.calculatePaneScaffoldDirective
import androidx.compose.runtime.Composable
import androidx.compose.runtime.CompositionLocalProvider
import androidx.compose.runtime.compositionLocalOf
import androidx.compose.runtime.remember
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.semantics.paneTitle
import androidx.compose.ui.semantics.semantics
import androidx.compose.ui.text.style.TextAlign
import androidx.compose.ui.unit.Dp
import androidx.compose.ui.unit.dp
import org.gaindrive.android.ui.Route
import kotlin.math.min

/*
 * The pane strip: `web/app.js`'s `paneNav`, in Compose.
 *
 * The web client is three fixed slots side by side, of which a window slides
 * across as the user drills in — the visible count a function of the content
 * width, and the level being read always the rightmost visible one. That is
 * also exactly what Material 3's [ListDetailPaneScaffold] draws, so the two
 * agree without either being bent to fit: a level is a pane role (0 List, 1
 * Detail, 2 Extra), and a role that has slid off the leading edge is
 * [PaneAdaptedValue.Hidden].
 *
 * What is deliberately *not* here is `ThreePaneScaffoldNavigator`. It keeps a
 * destination history of its own, which beside a tab's [PaneStack] would be a
 * second back stack — two things to keep in step through forward, back, a tab
 * switch, a resize and process death. Everything below is derived from the
 * stack per frame instead, so there is nothing to drift.
 */

/** Below this, one pane; the web client's own threshold for its second. */
private val TWO_PANE_MIN = 650.dp

/** And its third. */
private val THREE_PANE_MIN = 900.dp

/**
 * How many panes fit in [paneAreaWidth].
 *
 * The thresholds are the web client's, and they are measured on the **pane
 * area** rather than on the window, which is the same thing `paneNav` measures
 * (`#pane-viewport`, inside the 220px sidebar). A navigation rail takes about
 * 80dp, so keying this off the window would cost a tablet its third pane at
 * exactly the width the third pane exists for.
 *
 * They are not Material 3's 840/1600 window breakpoints, and that is a choice
 * rather than an oversight: 1600dp would mean no tablet made ever shows three
 * panes, while a 1280dp tablet in landscape gives three panes about 400dp each,
 * which is what the web has been doing for as long as it has had them.
 */
fun paneCount(paneAreaWidth: Dp): Int = when {
	paneAreaWidth >= THREE_PANE_MIN -> 3
	paneAreaWidth >= TWO_PANE_MIN -> 2
	else -> 1
}

/**
 * Note which role is which: [ListDetailPaneScaffold] calls its leading pane
 * List and maps it to *secondary*, and its middle pane Detail and maps it to
 * *primary*. Getting that pair the obvious way round silently draws the strip
 * inside out.
 */
@OptIn(ExperimentalMaterial3AdaptiveApi::class)
private fun PaneSlots.scaffoldValue(): ThreePaneScaffoldValue = ThreePaneScaffoldValue(
	secondary = list.adaptedValue(),
	primary = detail.adaptedValue(),
	tertiary = extra.adaptedValue(),
)

@OptIn(ExperimentalMaterial3AdaptiveApi::class)
private fun Pane.adaptedValue(): PaneAdaptedValue =
	if (this is Pane.Gone) PaneAdaptedValue.Hidden else PaneAdaptedValue.Expanded
// Note Blank is Expanded: holding its width is the whole reason it exists.

/**
 * The stock directive with the partition count and the pane widths replaced.
 *
 * `copy` rather than a fresh [PaneScaffoldDirective] because the default
 * carries `excludedBounds` — the bounds of a foldable's hinge, which is what
 * stops a pane being laid out across it. Building one from scratch loses that
 * silently, on the one class of device where it is visible.
 *
 * The widths are replaced because the stock ones are not equal and not even
 * consistent. The scaffold gives every pane a preferred 360dp and then hands
 * *all* surplus to its highest-priority pane — the detail — so a 900dp pane
 * area draws list 360 / detail 540; only in deficit does it scale the panes
 * evenly, which is why narrow windows looked right while tablets did not.
 * And the stock gutter keys on the *window* size class (0dp below EXPANDED,
 * 24dp above) while [paneCount] keys on the pane area at 650dp, so the gap
 * between two panes appeared and vanished with the window. The web client
 * divides `#pane-viewport` into exact equal panes with no gutter, each pane
 * padding its own content (`paneNav._apply()`), and the screens here pad
 * their own content the same way — so the directive says the same thing:
 * preferred widths that sum to the whole area, leaving no surplus for the
 * priority rule to misplace.
 *
 * Dividing by [panes] is exact, not approximate, because the window rules
 * keep every visible window full: [leadingWindow] and [searchWindow] mark a
 * level too deep to choose yet as [Pane.Blank] rather than [Pane.Gone], so
 * the number of expanded panes always equals [panes] and the widths are a
 * function of the window and the tab alone — they must never jump as the
 * user drills in. PaneWindowTest pins that invariant.
 */
@OptIn(ExperimentalMaterial3AdaptiveApi::class)
@Composable
fun paneDirective(panes: Int, paneAreaWidth: Dp): PaneScaffoldDirective =
	calculatePaneScaffoldDirective(currentWindowAdaptiveInfo())
		.copy(
			maxHorizontalPartitions = panes,
			horizontalPartitionSpacerSize = 0.dp,
			defaultPanePreferredWidth = paneAreaWidth / panes,
		)

/**
 * Draws [stack] as a strip of panes, one per level of its path.
 *
 * [titles] names each level, leading first, and its *size* is how deep the tab
 * goes — two for Playlists, three for the Library and Recents. One list rather
 * than a count and a lookup because the two could not then disagree. The names
 * reach the panes as `paneTitle`, which is what TalkBack announces when a pane
 * changes underneath the user; on a phone only one pane exists and it is the
 * screen, so this is a large-screen affordance specifically.
 *
 * [slots] is the mapping from "how deep are we, and how many panes fit" to what
 * each role shows. It is a parameter because Search needs a different one —
 * see [searchWindow], which is the only other implementation.
 *
 * [pane] is handed one route and draws it. It should go through [PaneHost], so
 * that the screen gets a back stack entry of its own — see that function for
 * why that is not optional.
 */
@OptIn(ExperimentalMaterial3AdaptiveApi::class)
@Composable
fun PaneStrip(
	stack: PaneStack,
	titles: List<String>,
	modifier: Modifier = Modifier,
	slots: (depth: Int, panes: Int, levels: Int) -> PaneSlots = ::leadingWindow,
	waiting: @Composable (level: Int) -> Unit = {},
	pane: @Composable (Route) -> Unit,
) {
	// The pane area, not the window: the navigation rail is already outside
	// this. Subcomposition, but once per tab and only for the tab on screen.
	val levels = titles.size
	// Outside the subcomposition on purpose: what a hidden level keeps is a
	// fact about the tab, and must not be thrown away by a resize that changes
	// how many levels are drawn. See PaneRetention.
	val retention = rememberPaneRetention(stack.path)
	BoxWithConstraints(modifier) {
		val panes = min(paneCount(maxWidth), levels)
		val assigned = slots(stack.depth, panes, levels)
		val path = stack.path

		// The back affordance goes on the leftmost visible pane, and only when
		// that is not the tab's own root — the web client's rule for its
		// `.back-link` spans. At two or three panes up, the level above is on
		// screen beside this one and an arrow pointing at it would be noise.
		val leading = listOf(assigned.list, assigned.detail, assigned.extra)
			.indexOfFirst { it !is Pane.Gone }
		val backAt = leading.takeIf { it > 0 }
		// Remembered so the identity is stable: it reaches the panes through a
		// composition local, and a lambda rebuilt each frame would recompose them
		// for nothing.
		val back = remember(stack) { { stack.back() } }

		ListDetailPaneScaffold(
			directive = paneDirective(panes, maxWidth),
			value = assigned.scaffoldValue(),
			listPane = {
				AnimatedPane(modifier = Modifier.paneName(titles, 0)) {
					Slot(assigned.list, 0, backAt, back, path, retention, waiting, pane)
				}
			},
			detailPane = {
				AnimatedPane(modifier = Modifier.paneName(titles, 1)) {
					Slot(assigned.detail, 1, backAt, back, path, retention, waiting, pane)
				}
			},
			extraPane = {
				AnimatedPane(modifier = Modifier.paneName(titles, 2)) {
					Slot(assigned.extra, 2, backAt, back, path, retention, waiting, pane)
				}
			},
		)
	}
}

@Composable
private fun Slot(
	content: Pane,
	level: Int,
	backAt: Int?,
	back: () -> Unit,
	path: List<Route>,
	retention: PaneRetention,
	waiting: @Composable (level: Int) -> Unit,
	pane: @Composable (Route) -> Unit,
) {
	when (content) {
		is Pane.Gone -> Unit
		// The blank bar with nothing under it — present for its width alone.
		is Pane.Blank -> PaneWaiting("")
		is Pane.Waiting -> waiting(level)
		// Guarded because a pane and the stack can disagree for one frame while
		// a pop is recomposing, and an index past the end would be a crash where
		// the right answer is an empty pane a moment early.
		is Pane.At -> path.getOrNull(content.index)?.let { route ->
			CompositionLocalProvider(LocalPaneBack provides back.takeIf { level == backAt }) {
				retention.Retained(route) { pane(route) }
			}
		}
	}
}

/**
 * What this pane's app bar should put behind its back arrow, or null when it
 * should draw none.
 *
 * Ambient rather than a parameter because it is a fact about the *layout* —
 * whether the level above is already on screen — which changes as the window
 * resizes, while the pane's own graph must not. A tab declares its
 * destinations in a `NavGraphBuilder` lambda that `NavHost` remembers; a
 * lambda capturing a value that moves with the width would rebuild the graph
 * on a resize, which throws away exactly the back stack entry the pane exists
 * to hold.
 */
val LocalPaneBack = compositionLocalOf<(() -> Unit)?> { null }

/**
 * `getOrElse` rather than an index: a tab with two levels still declares three
 * pane roles, and the third is always [Pane.Gone] — but the modifier is built
 * before anything knows that.
 */
private fun Modifier.paneName(titles: List<String>, level: Int): Modifier =
	semantics { paneTitle = titles.getOrElse(level) { "" } }

/**
 * The stock placeholder: one line saying what the pane is waiting for.
 *
 * Under an empty [TopAppBar], because the pane beside this one draws a real
 * one: without it the idle pane starts flush at the top while its neighbour
 * carries a bar, and the mismatched top edges read as a broken layout rather
 * than an intentionally empty state. The bar is the stock component, not a
 * measured spacer, so it tracks the neighbours' height through inset and
 * font-scale changes by construction.
 */
@OptIn(ExperimentalMaterial3Api::class)
@Composable
fun PaneWaiting(text: String) {
	Scaffold(
		topBar = { TopAppBar(title = {}) },
	) { insets ->
		Box(
			modifier = Modifier.fillMaxSize().padding(insets),
			contentAlignment = Alignment.Center,
		) {
			Text(
				text = text,
				style = MaterialTheme.typography.bodyMedium,
				color = MaterialTheme.colorScheme.onSurfaceVariant,
				textAlign = TextAlign.Center,
				modifier = Modifier.padding(24.dp),
			)
		}
	}
}
