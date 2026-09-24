//	GainDrive for iOS
//	Copyright (C) 2026 Kasper Peeters
//
//	This file is part of GainDrive, distributed under the GNU General
//	Public License version 3 or later, with the Application Distribution
//	Exception in ios/LICENSE. See LICENSE at the repository root for the
//	full text of the GPL.

import Foundation
import Testing
import UIKit

/// Phase 0 has no behaviour to test, but it does have configuration that is
/// silently wrong when it is wrong: a plist key that never reaches the bundle,
/// or a resource that XcodeGen dropped, both look exactly like a working
/// build. These assertions exist so `make test` is a real check from the
/// first commit rather than a target waiting for phase 1.
struct BundleConfigurationTests {
	/// Debug and release share this identifier deliberately, so switching
	/// between them keeps the configured servers rather than installing a
	/// second, empty app. A suffix added later would be a silent migration.
	///
	/// Run under `make test-mac` this also pins the Catalyst build to the same
	/// identifier: Xcode's default would make it `maccatalyst.org.gaindrive.ios`,
	/// and the Mac app would then be a stranger to every id the iOS one stores.
	@Test func bundleIdentifierIsStable() {
		#expect(Bundle.main.bundleIdentifier == "org.gaindrive.ios")
	}

	/// Background audio is the one background mode the app claims.
	///
	/// **Exactly, not merely containing.** Casting adds none: a direct cast
	/// keeps playing when the app is suspended because the receiver pulls from
	/// the server itself, and keeping a silent audio session alive to hold the
	/// app awake is fragile and a review risk. If this ever needs to change,
	/// that reasoning has to change with it.
	@Test func backgroundAudioIsDeclared() {
		let modes = Bundle.main.object(forInfoDictionaryKey: "UIBackgroundModes") as? [String]
		#expect(modes == ["audio"])
	}

	/// Browsing for Cast receivers finds nothing and reports no error when the
	/// service is not declared, which is indistinguishable from a network with
	/// no receivers on it - so the spelling is asserted rather than trusted.
	@Test func castBonjourServiceIsDeclared() {
		let services = Bundle.main.object(forInfoDictionaryKey: "NSBonjourServices") as? [String]
		#expect(services?.contains(CastDiscovery.serviceType) == true)
	}

	/// Self-hosted servers are reached over plain HTTP at addresses that
	/// cannot be pinned; without this the app cannot talk to most of them.
	@Test func arbitraryLoadsArePermitted() {
		let ats = Bundle.main.object(forInfoDictionaryKey: "NSAppTransportSecurity") as? [String: Any]
		#expect(ats?["NSAllowsArbitraryLoads"] as? Bool == true)
		#expect(ats?["NSAllowsLocalNetworking"] as? Bool == true)
	}

	/// The Local Network prompt is unavoidable for a LAN server, and an
	/// absent description string makes the connection fail rather than ask.
	@Test func localNetworkUsageIsDescribed() {
		let text = Bundle.main.object(forInfoDictionaryKey: "NSLocalNetworkUsageDescription") as? String
		#expect(text?.isEmpty == false)
	}

	/// A privacy manifest missing from the bundle is only reported at upload
	/// time, which is the worst moment to discover it.
	@Test func privacyManifestIsBundled() {
		#expect(Bundle.main.url(forResource: "PrivacyInfo", withExtension: "xcprivacy") != nil)
	}

	/// Named from the web client's palette, so the app is recognisably the
	/// same product. A typo in the colour set name degrades to the system
	/// blue without any error.
	@Test func accentColourResolves() {
		#expect(UIColor(named: "AccentColor") != nil)
	}

	/// Same silent failure as the colour, and worse to spot: SwiftUI's
	/// `Image("Logo")` renders an empty space for a missing asset rather than
	/// complaining, so a renamed or dropped image set looks like a layout bug.
	@Test func logoResolves() {
		#expect(UIImage(named: "Logo") != nil)
	}
}
