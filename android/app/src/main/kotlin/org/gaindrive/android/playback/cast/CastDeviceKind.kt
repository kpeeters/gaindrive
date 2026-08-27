package org.gaindrive.android.playback.cast

/**
 * What sort of receiver a [CastDevice] is, as far as its announced model says.
 *
 * [WIIM] is here because a WiiM speaker or amp is a Chromecast-built-in
 * receiver *and* carries a private HTTP API on the same address, offering
 * things Cast v2 has no message for. Recognising one is the prerequisite for
 * ever using it; nothing branches on this yet.
 */
enum class CastDeviceKind { GENERIC, WIIM }

/**
 * Classifies a receiver from its mDNS `md` record.
 *
 * A free function rather than a method on [CastDevice] so it can be tested
 * without an `NsdServiceInfo`: the parsing this feeds is a private extension
 * inside [CastDiscovery], and that is why nothing on the discovery path has a
 * test today.
 *
 * The match is a loose case-insensitive `contains` on purpose. WiiM ship at
 * least Mini, Pro, Pro Plus, Amp, Amp Pro and Ultra, and the model string is
 * whatever the firmware puts in a TXT record rather than anything specified —
 * their own HTTP API reports the same devices as `WiiM_AMP`, underscore and
 * all. A prefix or an exact table would quietly return [GENERIC] for a model
 * that had not been seen when this was written, which is the failure that
 * looks like the feature not working rather than like a missing case.
 */
fun castDeviceKind(model: String?): CastDeviceKind =
	if (model?.contains("wiim", ignoreCase = true) == true) {
		CastDeviceKind.WIIM
	} else {
		CastDeviceKind.GENERIC
	}

/** So no caller re-implements the test against [CastDevice.model]. */
val CastDevice.kind: CastDeviceKind get() = castDeviceKind(model)
