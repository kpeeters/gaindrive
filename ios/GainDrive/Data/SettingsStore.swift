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

	/// The browse scope, as `BrowseScope.stored` — a server's UUID or the
	/// `"all"` sentinel. Kept as a string rather than as a `BrowseScope`
	/// because resolving it needs the current server list, which this type
	/// deliberately knows nothing about. `ServerSelection` does the resolving.
	var selectedServer: String? {
		didSet { defaults.set(selectedServer, forKey: Self.selectedServerKey) }
	}

	/// **On by default here, off by default on Android.** The two apps
	/// genuinely differ: `ios/PLAN.md` says "Albums merge, on by default" and
	/// `android/data/Merge.kt` says the opposite. Recorded so the next reader
	/// does not "fix" one of them into agreement.
	var mergeDuplicateAlbums: Bool {
		didSet { defaults.set(mergeDuplicateAlbums, forKey: Self.mergeAlbumsKey) }
	}

	/// One global quality, capped per track by *that track's* account ceiling —
	/// a queue spanning servers crosses caps at every boundary, so this is
	/// deliberately not per server.
	var audioQuality: AudioQuality {
		didSet { defaults.set(audioQuality.tag, forKey: Self.audioQualityKey) }
	}

	@ObservationIgnored private let defaults: UserDefaults
	// Spelled as Android spells them in its DataStore, so the two apps
	// describe the same settings by the same names.
	private static let themeKey = "theme_mode"
	private static let selectedServerKey = "selected_server"
	private static let mergeAlbumsKey = "merge_duplicate_albums"
	private static let audioQualityKey = "audio_quality"

	init(defaults: UserDefaults = .standard) {
		self.defaults = defaults
		let stored = defaults.string(forKey: Self.themeKey)
		themeMode = stored.flatMap(ThemeMode.init(rawValue:)) ?? .auto
		selectedServer = defaults.string(forKey: Self.selectedServerKey)
		// `bool(forKey:)` answers false for an absent key, so the default has
		// to be read through `object(forKey:)` or a fresh install would get
		// the opposite of what is intended.
		mergeDuplicateAlbums =
			defaults.object(forKey: Self.mergeAlbumsKey) as? Bool ?? true
		// Stored as the tag so it is a single value — a format and a bitrate
		// that could disagree would be two settings pretending to be one.
		audioQuality =
			defaults.string(forKey: Self.audioQualityKey).flatMap(AudioQuality.parse) ?? .default
	}
}
