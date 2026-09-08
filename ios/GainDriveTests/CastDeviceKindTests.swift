//	GainDrive for iOS
//	Copyright (C) 2026 Kasper Peeters
//
//	This file is part of GainDrive, distributed under the GNU General
//	Public License version 3 or later, with the Application Distribution
//	Exception in ios/LICENSE. See LICENSE at the repository root for the
//	full text of the GPL.

import Testing

@testable import GainDrive

/// Telling a WiiM from anything else by its announced model.
///
/// The match is loose on purpose, and the reason is the failure mode of the
/// alternative: an exact table returns `generic` for a model that did not exist
/// when it was written, and that reads as the feature being broken rather than
/// as a missing case.
struct CastDeviceKindTests {
	@Test(
		arguments: [
			"WiiM Mini", "WiiM Pro", "WiiM Pro Plus", "WiiM Amp", "WiiM Amp Pro", "WiiM Ultra",
			// Their own HTTP API spells the same devices this way, underscore
			// and all, so the matcher must not depend on the space.
			"WiiM_AMP",
			// A model that does not exist yet. The point of the loose match is
			// that this still answers correctly.
			"WiiM Amp Ultra Pro Max",
			"wiim pro",
		])
	func aWiiMIsRecognised(_ model: String) {
		#expect(castDeviceKind(model) == .wiim)
	}

	@Test(arguments: ["Chromecast", "Chromecast Ultra", "Google Home Mini", "SHIELD Android TV", ""])
	func everythingElseIsGeneric(_ model: String) {
		#expect(castDeviceKind(model) == .generic)
	}

	/// A manually added device has no announcement to read.
	@Test func noModelIsGeneric() {
		#expect(castDeviceKind(nil) == .generic)
	}
}
