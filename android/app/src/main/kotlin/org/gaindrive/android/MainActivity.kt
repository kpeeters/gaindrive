package org.gaindrive.android

import android.Manifest
import android.content.Intent
import android.os.Build
import android.os.Bundle
import androidx.activity.ComponentActivity
import androidx.activity.compose.rememberLauncherForActivityResult
import androidx.activity.compose.setContent
import androidx.activity.enableEdgeToEdge
import androidx.activity.result.contract.ActivityResultContracts
import androidx.compose.runtime.CompositionLocalProvider
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.remember
import androidx.hilt.lifecycle.viewmodel.compose.hiltViewModel
import androidx.lifecycle.compose.collectAsStateWithLifecycle
import dagger.hilt.android.AndroidEntryPoint
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.asStateFlow
import org.gaindrive.android.data.PairLink
import org.gaindrive.android.data.TrackLink
import org.gaindrive.android.data.extractSharedUrl
import org.gaindrive.android.data.parsePairLink
import org.gaindrive.android.data.parseTrackLink
import org.gaindrive.android.ui.AvailabilityViewModel
import org.gaindrive.android.ui.GainDriveApp
import org.gaindrive.android.ui.LocalAvailability
import org.gaindrive.android.ui.TvEnvironment
import org.gaindrive.android.ui.isTvDevice
import org.gaindrive.android.ui.settings.SettingsViewModel
import org.gaindrive.android.ui.theme.GainDriveTheme

@AndroidEntryPoint
class MainActivity : ComponentActivity() {

	/**
	 * A URL shared with the app and not yet shown, or null.
	 *
	 * Held here rather than read from `getIntent()` by the composables, because
	 * an intent is not consumed by being read: every recomposition would see it
	 * again. This is set once per arriving share and cleared by whoever acts on
	 * it.
	 */
	private val sharedUrl = MutableStateFlow<String?>(null)

	/**
	 * A gaindrive:// track link and not yet acted on, or null. The same
	 * holder-not-intent bargain as [sharedUrl], for the same reason.
	 */
	private val trackLink = MutableStateFlow<TrackLink?>(null)

	/**
	 * A gaindrive://pair link - a TV's QR, scanned - not yet confirmed or
	 * declined. Third holder, same bargain.
	 */
	private val pairLink = MutableStateFlow<PairLink?>(null)

	override fun onCreate(savedInstanceState: Bundle?) {
		super.onCreate(savedInstanceState)
		enableEdgeToEdge()

		// Only on a genuinely new instance. A configuration change or a restore
		// after process death arrives with the same intent still attached, and
		// the navigation back stack has already been restored with the panel on
		// it - reading the intent again would push a second copy of it, in front
		// of a user who may have backed out of the first.
		if (savedInstanceState == null) {
			takeSharedUrl(intent)
			takeTrackLink(intent)
			takePairLink(intent)
		}

		setContent {
			// One view model supplies both the theme and the server list, so
			// the theme cannot lag a change to the setting.
			val viewModel: SettingsViewModel = hiltViewModel()
			val state by viewModel.state.collectAsStateWithLifecycle()
			val isTv = remember { isTvDevice(applicationContext) }

			if (!isTv) {
				// A TV has no notification shade, so the prompt would be the
				// first thing a remote user sees and grant nothing visible.
				RequestNotificationPermission()
			}

			// Collected once, at the root, and made ambient: every track row
			// wants to know whether its audio is stored and whether there is a
			// network, and nothing in between has anything to say about it.
			val availability: AvailabilityViewModel = hiltViewModel()
			val availabilityState by availability.state.collectAsStateWithLifecycle()

			GainDriveTheme(mode = state.themeMode, isTv = isTv) {
				// Inside the theme, so the TV indication wraps the themed ripple.
				TvEnvironment(isTv) {
					CompositionLocalProvider(LocalAvailability provides availabilityState) {
						GainDriveApp(
							settingsViewModel = viewModel,
							sharedUrl = sharedUrl.asStateFlow(),
							onSharedUrlHandled = { sharedUrl.value = null },
							trackLink = trackLink.asStateFlow(),
							onTrackLinkHandled = { trackLink.value = null },
							pairLink = pairLink.asStateFlow(),
							onPairLinkHandled = { pairLink.value = null },
						)
					}
				}
			}
		}
	}

	/**
	 * A share arriving while the app is already running.
	 *
	 * `launchMode="singleTop"` is what routes it here rather than stacking a
	 * second copy of this activity: this is the only activity, so it is always
	 * the top of whatever task the share sheet finds, and the intent is
	 * therefore delivered to the instance that already exists.
	 */
	override fun onNewIntent(intent: Intent) {
		super.onNewIntent(intent)
		// Not optional, and its absence is silent: without it `getIntent()` goes
		// on returning the intent that first started the activity, so anything
		// reading it after a configuration change sees the wrong one.
		setIntent(intent)
		takeSharedUrl(intent)
		takeTrackLink(intent)
		takePairLink(intent)
	}

	private fun takeSharedUrl(intent: Intent?) {
		if (intent?.action != Intent.ACTION_SEND) return
		// Shared text is very often a title and then a link, so a URL is
		// extracted rather than taken whole. Text with no URL in it leaves this
		// null and nothing happens, which is the right answer for a paragraph of
		// prose sent here by mistake.
		extractSharedUrl(intent.getStringExtra(Intent.EXTRA_TEXT))?.let {
			sharedUrl.value = it
		}
	}

	private fun takeTrackLink(intent: Intent?) {
		if (intent?.action != Intent.ACTION_VIEW) return
		// The filter admits only the scheme, but an intent is whatever another
		// app says it is: a URI that does not parse to a track is ignored, the
		// answer extractSharedUrl gives prose with no link in it.
		parseTrackLink(intent.data?.toString())?.let {
			trackLink.value = it
		}
	}

	private fun takePairLink(intent: Intent?) {
		if (intent?.action != Intent.ACTION_VIEW) return
		// Shares the one VIEW filter with track links; the parsers split the
		// scheme between them by authority, so at most one of these takes.
		parsePairLink(intent.data?.toString())?.let {
			pairLink.value = it
		}
	}
}

/**
 * Asks for POST_NOTIFICATIONS on API 33+, where the media notification needs it.
 *
 * Denial is not an error: playback works either way, the notification is simply
 * silent. So this asks once and never nags.
 */
@androidx.compose.runtime.Composable
private fun RequestNotificationPermission() {
	if (Build.VERSION.SDK_INT < Build.VERSION_CODES.TIRAMISU) return

	val launcher = rememberLauncherForActivityResult(
		contract = ActivityResultContracts.RequestPermission(),
		onResult = { /* Either way, playback is unaffected. */ },
	)
	LaunchedEffect(Unit) {
		launcher.launch(Manifest.permission.POST_NOTIFICATIONS)
	}
}
