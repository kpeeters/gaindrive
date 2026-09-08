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

	/// Stored music only — no server is contacted at all.
	///
	/// **A mode, not a display filter.** The repository skips the request
	/// rather than making it and hiding the answer: a manual offline mode on a
	/// flaky connection would otherwise be worse than useless, waiting out a
	/// full timeout before showing the mirror it could have shown at once.
	///
	/// It lives in the library picker rather than in Settings because it
	/// answers the same question the scope does, and because it is flipped
	/// before a flight rather than configured once.
	var offlineMode: Bool {
		didSet { defaults.set(offlineMode, forKey: Self.offlineKey) }
	}

	/// Which slice of the library was last showing, as `LibraryMode.id`.
	///
	/// Stored as the raw string rather than as a `LibraryMode`, so a value
	/// written by a build that knew a kind this one does not still parses —
	/// which is the same reason `LibraryMode` wraps a string at all. An
	/// unrecognised one falls back when the chip row is read.
	var libraryMode: String? {
		didSet { defaults.set(libraryMode, forKey: Self.libraryModeKey) }
	}

	/// Album order, **per slice**, so films can sit A–Z while a musician's
	/// albums stay chronological. A single global setting would make one of
	/// those two wrong every time the other was set.
	private(set) var albumSorts: [String: String] {
		didSet { defaults.set(albumSorts, forKey: Self.albumSortsKey) }
	}

	func albumSort(for mode: LibraryMode) -> AlbumSort {
		AlbumSort.parse(albumSorts[mode.id])
	}

	func setAlbumSort(_ sort: AlbumSort, for mode: LibraryMode) {
		albumSorts[mode.id] = sort.rawValue
	}

	/// How much downloaded audio may sit on the device.
	///
	/// **A ceiling on a refusal, not on an evictor** — for now. Nothing here is
	/// cached-on-play yet, so everything stored was explicitly asked for and
	/// nothing may be reclaimed; pinning past the cap is refused instead. When
	/// cache-on-play arrives this becomes the evictor's bound as well, and the
	/// refusal stays, because eviction cannot reclaim pinned bytes.
	///
	/// 4 GB to match Android's `DEFAULT_CACHE_BYTES`. Stored as a `Double`
	/// because `UserDefaults` has no `Int64` accessor and a music library
	/// passes what an `Int` is guaranteed to hold on a 32-bit device.
	var cacheCapBytes: Int64 {
		didSet { defaults.set(Double(cacheCapBytes), forKey: Self.cacheCapKey) }
	}

	static let defaultCacheCapBytes: Int64 = 4 * 1024 * 1024 * 1024
	static let cacheCapChoices: [Int64] = [
		1 * 1024 * 1024 * 1024,
		2 * 1024 * 1024 * 1024,
		4 * 1024 * 1024 * 1024,
		8 * 1024 * 1024 * 1024,
		16 * 1024 * 1024 * 1024,
	]

	@ObservationIgnored private let defaults: UserDefaults
	// Spelled as Android spells them in its DataStore, so the two apps
	// describe the same settings by the same names.
	private static let themeKey = "theme_mode"
	private static let selectedServerKey = "selected_server"
	private static let mergeAlbumsKey = "merge_duplicate_albums"
	private static let audioQualityKey = "audio_quality"
	private static let libraryModeKey = "library_mode"
	private static let albumSortsKey = "album_sort"
	private static let cacheCapKey = "cache_max_bytes"
	private static let offlineKey = "offline_mode"

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
		libraryMode = defaults.string(forKey: Self.libraryModeKey)
		albumSorts = defaults.dictionary(forKey: Self.albumSortsKey) as? [String: String] ?? [:]
		// `double(forKey:)` answers 0 for an absent key, which would be a cap
		// of nothing rather than the default — the same trap the merge switch
		// above avoids with `object(forKey:)`.
		offlineMode = defaults.bool(forKey: Self.offlineKey)
		let storedCap = defaults.object(forKey: Self.cacheCapKey) as? Double
		cacheCapBytes = storedCap.map { Int64($0) } ?? Self.defaultCacheCapBytes
	}
}
