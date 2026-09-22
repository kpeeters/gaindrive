package org.gaindrive.android.ui

import android.Manifest
import android.content.pm.PackageManager
import android.os.Build
import androidx.activity.compose.rememberLauncherForActivityResult
import androidx.activity.result.contract.ActivityResultContracts
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.platform.LocalContext

/**
 * Whether ACCESS_LOCAL_NETWORK is held, asking for it once when it is not.
 *
 * Android 17 gates every LAN socket behind this runtime permission when
 * targeting SDK 37, and an app that never requests it gets a session consent
 * dialog from the system on every single scan. Asked the ordinary way instead,
 * one grant persists. Below API 37 the permission does not exist and the
 * answer is simply true.
 *
 * Null while the system dialog is up, so a caller can hold its tongue rather
 * than flash a denial message that is about to become a grant. After a
 * permanent denial the launch below returns false immediately and shows
 * nothing, so calling this on every open does not nag.
 */
@Composable
fun rememberLocalNetworkPermission(): Boolean? {
	// No named VERSION_CODES constant is relied on for Android 17; the number
	// is the stable fact.
	if (Build.VERSION.SDK_INT < 37) return true

	val context = LocalContext.current
	var granted by remember {
		mutableStateOf<Boolean?>(
			if (context.checkSelfPermission(Manifest.permission.ACCESS_LOCAL_NETWORK) ==
				PackageManager.PERMISSION_GRANTED
			) true else null,
		)
	}
	val launcher = rememberLauncherForActivityResult(
		contract = ActivityResultContracts.RequestPermission(),
		onResult = { granted = it },
	)
	LaunchedEffect(Unit) {
		if (granted != true) launcher.launch(Manifest.permission.ACCESS_LOCAL_NETWORK)
	}
	return granted
}
