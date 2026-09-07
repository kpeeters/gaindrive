//	GainDrive for iOS
//	Copyright (C) 2026 Kasper Peeters
//
//	This file is part of GainDrive, distributed under the GNU General
//	Public License version 3 or later, with the Application Distribution
//	Exception in ios/LICENSE. See LICENSE at the repository root for the
//	full text of the GPL.

import Foundation

/// When a play counts as having happened.
///
/// Its own type, and not a static on `Scrobbler`, for the reason `QueueMove`
/// and `PlayerWindow` are: it is the one rule here worth arguing about, and
/// pulling it out is what lets it be tested by calling it rather than by
/// driving a player. It also keeps it off the main actor, where a pure
/// function has no business being.
enum ScrobbleRule {
	/// **Half the track, or four minutes, whichever comes first** — the
	/// convention scrobbling services have used for years. The second half of
	/// it is what stops a long track having to finish before it counts.
	static let submitAfter: Double = 4 * 60

	/// A duration of zero is "not known yet", **not** "already past half":
	/// every position is past half of nothing, so without the guard the first
	/// tick of every track would submit it.
	static func shouldSubmit(position: Double, duration: Double) -> Bool {
		guard duration > 0, position.isFinite, position > 0 else { return false }
		return position >= duration / 2 || position >= submitAfter
	}
}

/// Tells the server what is playing, and what has been played.
///
/// Without it the server's play counts and `last_played` are never written, so
/// `getRecentSongs` stays empty for anything played here — **including in the
/// web and Android clients**, which read the same server-side state. That is
/// why this is a correctness port rather than a feature.
///
/// Attached to `PlayerConnection` rather than to the `AVQueuePlayer`.
/// `PLAN.md` states the rule and Android is where it was learned: scrobbling
/// was attached to the `ExoPlayer` there and went silent the moment playback
/// went remote. Phase 9 swaps the player; this must not notice.
@MainActor
final class Scrobbler {
	private let library: LibraryRepository
	private var current: ItemRef?
	/// Per track, and the reason seeking backwards across the halfway mark does
	/// not submit a second time.
	private var submitted = false

	init(library: LibraryRepository) {
		self.library = library
	}

	/// Sends the now-playing notification and re-arms for the new track.
	///
	/// Called with `nil` when the queue empties, so a later tick cannot submit
	/// a play for a track that is no longer current.
	func trackChanged(to ref: ItemRef?) {
		current = ref
		submitted = false
		guard let ref else { return }
		// To *that track's* server. `LibraryRepository` resolves the client
		// from the ref, so nothing here closes over a current server — the same
		// property that makes a mixed-server queue play.
		Task { await library.scrobble(ref, submission: false) }
	}

	/// Driven from the periodic time observer, which only fires while the
	/// timeline is advancing — so this needs no "is it playing" guard of its
	/// own, unlike Android's, which polls on a timer of its own.
	func tick(position: Double, duration: Double) {
		guard !submitted, let ref = current else { return }
		guard ScrobbleRule.shouldSubmit(position: position, duration: duration) else { return }
		submitted = true
		Task { await library.scrobble(ref, submission: true) }
	}
}
