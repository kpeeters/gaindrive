package org.gaindrive.android.ui.theme

import androidx.compose.foundation.isSystemInDarkTheme
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.darkColorScheme
import androidx.compose.material3.lightColorScheme
import androidx.compose.runtime.Composable
import androidx.compose.ui.graphics.Color
import org.gaindrive.android.data.model.ThemeMode

// Taken from web/style.css so the app and the web client are recognisably the
// same product, rather than following the system wallpaper.
//
// The accent differs between schemes on purpose: #a31623 does not have enough
// contrast against #1a1a1a, so the dark scheme lifts it. That is the web
// client's choice too, not an invention here.

private val AccentLight = Color(0xFFA31623)
private val AccentDark = Color(0xFFC9202F)
private val Sand = Color(0xFFF4D58D)

private val LightScheme = lightColorScheme(
	primary = AccentLight,
	onPrimary = Color.White,
	primaryContainer = Sand,
	onPrimaryContainer = Color(0xFF1A1A1A),
	secondary = AccentLight,
	onSecondary = Color.White,
	background = Color(0xFFF4F4F4),
	onBackground = Color(0xFF1A1A1A),
	surface = Color(0xFFFFFFFF),
	onSurface = Color(0xFF1A1A1A),
	surfaceVariant = Color(0xFFF4F4F4),
	onSurfaceVariant = Color(0xFF666666),
	outline = Color(0xFFBBBBBB),
	outlineVariant = Color(0xFFDDDDDD),
	error = Color(0xFFB3261E),
	onError = Color.White,
)

private val DarkScheme = darkColorScheme(
	primary = AccentDark,
	onPrimary = Color.White,
	primaryContainer = Color(0xFF5A0E17),
	onPrimaryContainer = Sand,
	secondary = AccentDark,
	onSecondary = Color.White,
	background = Color(0xFF1A1A1A),
	onBackground = Color(0xFFE0E0E0),
	surface = Color(0xFF242424),
	onSurface = Color(0xFFE0E0E0),
	surfaceVariant = Color(0xFF242424),
	onSurfaceVariant = Color(0xFF888888),
	outline = Color(0xFF505050),
	outlineVariant = Color(0xFF333333),
	error = Color(0xFFE05555),
	onError = Color(0xFF1A1A1A),
)

// The dark scheme with its dim roles lifted for a television. A TV panel
// crushes the low greys a phone renders faithfully: #505050 outlines and
// #888888 secondary text, fine at arm's length, read as near-black from a
// sofa. Only the roles that sit close to the background move; onSurface and
// the accent were already bright enough to survive the panel.
private val TvDarkScheme = DarkScheme.copy(
	onSurfaceVariant = Color(0xFFB4B4B4),
	outline = Color(0xFF8A8A8A),
	outlineVariant = Color(0xFF5A5A5A),
)

@Composable
fun GainDriveTheme(
	mode: ThemeMode = ThemeMode.AUTO,
	isTv: Boolean = false,
	content: @Composable () -> Unit,
) {
	val dark = when (mode) {
		ThemeMode.AUTO -> isSystemInDarkTheme()
		ThemeMode.LIGHT -> false
		ThemeMode.DARK -> true
	}
	MaterialTheme(
		colorScheme = when {
			dark && isTv -> TvDarkScheme
			dark -> DarkScheme
			else -> LightScheme
		},
		content = content,
	)
}
