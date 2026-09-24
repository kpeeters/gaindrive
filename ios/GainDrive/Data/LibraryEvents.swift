//	GainDrive for iOS
//	Copyright (C) 2026 Kasper Peeters
//
//	This file is part of GainDrive, distributed under the GNU General
//	Public License version 3 or later, with the Application Distribution
//	Exception in ios/LICENSE. See LICENSE at the repository root for the
//	full text of the GPL.

import Foundation

/// Changes one screen makes that another screen has to notice.
///
/// Android hangs this counter on the repository itself as a `StateFlow`.
/// `LibraryRepository` here is `Sendable` and deliberately not isolated -
/// which is what lets the fan-out leave the main actor - so it cannot hold
/// observable state, and the counter lives in its own small type instead.
///
/// A counter rather than a list of what changed: the screens that care re-read
/// anyway, and a description of the change would be a second source of truth
/// about the playlist that the reload then contradicts.
@MainActor
@Observable
final class LibraryEvents {
	/// Bumped after every successful playlist write, wherever it happened.
	///
	/// A track added from an album detail three screens away has to appear in
	/// the playlist without the user pulling to refresh - and, more sharply,
	/// the playlists list must not keep showing a playlist that was just
	/// deleted from inside it.
	private(set) var playlistRevision = 0

	func playlistsChanged() {
		playlistRevision += 1
	}
}
