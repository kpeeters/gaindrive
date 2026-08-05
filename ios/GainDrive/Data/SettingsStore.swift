//	GainDrive for iOS
//	Copyright (C) 2026 Kasper Peeters
//
//	This file is part of GainDrive, distributed under the GNU General
//	Public License version 3 or later, with the Application Distribution
//	Exception in ios/LICENSE. See LICENSE at the repository root for the
//	full text of the GPL.

import Foundation

/// Settings that are not about any one server.
///
/// Deliberately Foundation-only — the mapping from `ThemeMode` to SwiftUI's
/// `ColorScheme` lives in the UI layer, so the dependency direction stays
/// `UI → Data → Net`.
@MainActor
@Observable
final class SettingsStore {
	var themeMode: ThemeMode {
		didSet { defaults.set(themeMode.rawValue, forKey: Self.themeKey) }
	}

	@ObservationIgnored private let defaults: UserDefaults
	// Spelled as Android spells it in its DataStore, so the two apps
	// describe the same setting by the same name.
	private static let themeKey = "theme_mode"

	init(defaults: UserDefaults = .standard) {
		self.defaults = defaults
		let stored = defaults.string(forKey: Self.themeKey)
		themeMode = stored.flatMap(ThemeMode.init(rawValue:)) ?? .auto
	}
}
