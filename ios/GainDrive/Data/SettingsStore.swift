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
/// Deliberately Foundation-only - the mapping from `ThemeMode` to SwiftUI's
/// `ColorScheme` lives in the UI layer, so the dependency direction stays
/// `UI → Data → Net`.
@MainActor
@Observable
final class SettingsStore {
	var themeMode: ThemeMode {
		didSet { defaults.set(themeMode.rawValue, forKey: Self.themeKey) }
	}

	/// The browse scope, as `BrowseScope.stored` - a server's UUID or the
	/// `"all"` sentinel. Kept as a string rather than as a `BrowseScope`
	/// because resolving it needs the current server list, which this type
	/// deliberately knows nothing about. `ServerSelection` does the resolving.
	var selectedServer: String? {
		didSet { defaults.set(selectedServer, forKey: Self.selectedServerKey) }
	}

	/// **On by default here, off by default on Android.** The two apps
	/// genuinely differ: merging is on by default here, and
	/// `android/data/Merge.kt` says the opposite. Recorded so the next reader
	/// does not "fix" one of them into agreement.
	var mergeDuplicateAlbums: Bool {
		didSet { defaults.set(mergeDuplicateAlbums, forKey: Self.mergeAlbumsKey) }
	}

	/// One global quality, capped per track by *that track's* account ceiling -
	/// a queue spanning servers crosses caps at every boundary, so this is
	/// deliberately not per server.
	var audioQuality: AudioQuality {
		didSet { defaults.set(audioQuality.tag, forKey: Self.audioQualityKey) }
	}

	/// Stored music only - no server is contacted at all.
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

	/// Album order, **per library section**, so films can sit A–Z while a
	/// musician's albums stay chronological. A single global setting would
	/// make one of those two wrong every time the other was set. The caller
	/// names the section it drilled in from - this used to key on a stored
	/// "current mode", which could disagree with the listing on screen.
	private(set) var albumSorts: [String: String] {
		didSet { defaults.set(albumSorts, forKey: Self.albumSortsKey) }
	}

	func albumSort(for section: LibrarySection) -> AlbumSort {
		AlbumSort.parse(albumSorts[section.rawValue])
	}

	func setAlbumSort(_ sort: AlbumSort, for section: LibrarySection) {
		albumSorts[section.rawValue] = sort.rawValue
	}

	/// How much audio may sit on the device.
	///
	/// **Two jobs.** It bounds the evictor, which removes music kept from
	/// playing, oldest first. And it refuses a download that would not fit,
	/// because eviction cannot reclaim pinned bytes and pinning past the cap
	/// would quietly turn it into a lie.
	///
	/// 4 GB to match Android's `DEFAULT_CACHE_BYTES`. Stored as a `Double`
	/// because `UserDefaults` has no `Int64` accessor and a music library
	/// passes what an `Int` is guaranteed to hold on a 32-bit device.
	var cacheCapBytes: Int64 {
		didSet { defaults.set(Double(cacheCapBytes), forKey: Self.cacheCapKey) }
	}

	/// Whether playing a track also keeps it.
	///
	/// On by default, as on Android: replaying an album is the common case,
	/// and the cap plus eviction stop it running away. Off, a track streams as
	/// it did before the cache existed, and what is already stored still plays.
	var cacheOnPlay: Bool {
		didSet { defaults.set(cacheOnPlay, forKey: Self.cacheOnPlayKey) }
	}

	/// Whether downloads wait for a network that is not metered.
	///
	/// On by default, as on Android. Only downloads: keeping what is already
	/// being streamed costs no extra data, so gating that would penalise the
	/// mobile listener for nothing. "Metered" is the system's `isExpensive`,
	/// which covers cellular and a personal hotspot.
	var downloadUnmeteredOnly: Bool {
		didSet { defaults.set(downloadUnmeteredOnly, forKey: Self.unmeteredOnlyKey) }
	}

	/// Play videos for their soundtrack alone: no picture, through the audio
	/// path, so a concert streams, caches and downloads like an album.
	///
	/// Global rather than per track, as on Android: a pin has to know before
	/// anything is fetched whether what it stores for a film is its soundtrack
	/// or nothing. Off by default - a video library is a video library until
	/// someone says otherwise.
	var videoAudioOnly: Bool {
		didSet { defaults.set(videoAudioOnly, forKey: Self.videoAudioOnlyKey) }
	}

	/// What the fetch form was last set to, so the next fetch starts there.
	/// The names are sticky because a run of fetches is usually one album's
	/// worth of tracks or one series' episodes.
	var fetchServer: String? {
		didSet { defaults.set(fetchServer, forKey: Self.fetchServerKey) }
	}
	var fetchAudio: Bool {
		didSet { defaults.set(fetchAudio, forKey: Self.fetchAudioKey) }
	}
	var fetchArtist: String {
		didSet { defaults.set(fetchArtist, forKey: Self.fetchArtistKey) }
	}
	var fetchAlbum: String {
		didSet { defaults.set(fetchAlbum, forKey: Self.fetchAlbumKey) }
	}

	/// Whether this song plays with a picture. **The one question** every
	/// "is it a video" decision in playback asks, so the setting cannot be
	/// honoured in one place and forgotten in another.
	///
	/// Always false on the Mac until it has a video surface of its own (phase
	/// M2 of `.ai/macos/PLAN.md`): a video there plays its soundtrack, exactly
	/// as with "Play videos as audio only".
	func showsPicture(_ song: Song) -> Bool {
		#if os(macOS)
			false
		#else
			song.isVideo && !videoAudioOnly
		#endif
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
	private static let albumSortsKey = "album_sort"
	private static let cacheCapKey = "cache_max_bytes"
	private static let offlineKey = "offline_mode"
	private static let cacheOnPlayKey = "cache_on_play"
	private static let unmeteredOnlyKey = "download_unmetered_only"
	private static let videoAudioOnlyKey = "video_audio_only"
	private static let fetchServerKey = "fetch_server"
	private static let fetchAudioKey = "fetch_audio"
	private static let fetchArtistKey = "fetch_artist"
	private static let fetchAlbumKey = "fetch_album"

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
		// Stored as the tag so it is a single value - a format and a bitrate
		// that could disagree would be two settings pretending to be one.
		audioQuality =
			defaults.string(forKey: Self.audioQualityKey).flatMap(AudioQuality.parse) ?? .default
		albumSorts = defaults.dictionary(forKey: Self.albumSortsKey) as? [String: String] ?? [:]
		// `double(forKey:)` answers 0 for an absent key, which would be a cap
		// of nothing rather than the default - the same trap the merge switch
		// above avoids with `object(forKey:)`.
		offlineMode = defaults.bool(forKey: Self.offlineKey)
		let storedCap = defaults.object(forKey: Self.cacheCapKey) as? Double
		cacheCapBytes = storedCap.map { Int64($0) } ?? Self.defaultCacheCapBytes
		cacheOnPlay = defaults.object(forKey: Self.cacheOnPlayKey) as? Bool ?? true
		downloadUnmeteredOnly = defaults.object(forKey: Self.unmeteredOnlyKey) as? Bool ?? true
		videoAudioOnly = defaults.bool(forKey: Self.videoAudioOnlyKey)
		fetchServer = defaults.string(forKey: Self.fetchServerKey)
		fetchAudio = defaults.object(forKey: Self.fetchAudioKey) as? Bool ?? true
		fetchArtist = defaults.string(forKey: Self.fetchArtistKey) ?? ""
		fetchAlbum = defaults.string(forKey: Self.fetchAlbumKey) ?? ""
	}
}
