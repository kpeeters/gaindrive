//	GainDrive for iOS
//	Copyright (C) 2026 Kasper Peeters
//
//	This file is part of GainDrive, distributed under the GNU General
//	Public License version 3 or later, with the Application Distribution
//	Exception in ios/LICENSE. See LICENSE at the repository root for the
//	full text of the GPL.

import Foundation

/// What sort of receiver a `CastDevice` is, as far as its announced model says.
///
/// `wiim` is here because a WiiM speaker or amp is a Chromecast-built-in
/// receiver *and* carries a private HTTP API on the same address, offering
/// things Cast v2 has no message for. Recognising one is the prerequisite for
/// ever using it; nothing branches on this yet.
enum CastDeviceKind: String, Hashable, Sendable {
	case generic, wiim
}

/// Classifies a receiver from its mDNS `md` record.
///
/// A free function rather than a method on `CastDevice` so it can be tested
/// without a browse result.
///
/// **The match is a loose case-insensitive `contains` on purpose.** WiiM ship at
/// least Mini, Pro, Pro Plus, Amp, Amp Pro and Ultra, and the model string is
/// whatever the firmware puts in a TXT record rather than anything specified —
/// their own HTTP API reports the same devices as `WiiM_AMP`, underscore and
/// all. A prefix or an exact table would quietly return `generic` for a model
/// that had not been seen when this was written, which is the failure that looks
/// like the feature not working rather than like a missing case.
func castDeviceKind(_ model: String?) -> CastDeviceKind {
	guard let model, model.range(of: "wiim", options: .caseInsensitive) != nil else {
		return .generic
	}
	return .wiim
}
