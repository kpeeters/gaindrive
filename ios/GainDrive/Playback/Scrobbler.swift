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
	/// **Half the track, or four minutes, whichever comes first** - the
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

/// What `reportPlayback` says this device is doing with the current track.
enum PlaybackState: String, Sendable {
	case starting, playing, paused, stopped
}

/// Tells the server what is playing, and what has been played.
///
/// Without it the server's play counts and `last_played` are never written, so
/// `getRecentSongs` stays empty for anything played here - **including in the
/// web and Android clients**, which read the same server-side state. That is
/// why this is a correctness port rather than a feature.
///
/// Attached to `PlayerConnection` rather than to the `AVQueuePlayer`.
/// Android is where the rule was learned: scrobbling
/// was attached to the `ExoPlayer` there and went silent the moment playback
/// went remote. Phase 9 swaps the player; this must not notice.
@MainActor
final class Scrobbler {
	private let library: LibraryRepository
	private var current: ItemRef?
	/// Per track, and the reason seeking backwards across the halfway mark does
	/// not submit a second time.
	private var submitted = false

	/// The last state sent for `current`; `nil` until the first. Lets a
	/// restart after "stopped" be announced again, and the refresh know
	/// whether there is anything to keep alive.
	private var reported: PlaybackState?
	private var reportedAt = ContinuousClock.now
	/// The transport as last published, which a held-back pause re-checks.
	private var playing = false
	private var position: Double = 0
	private var pendingPause: Task<Void, Never>?
	/// The previous report. Each waits for it, because they are separate
	/// requests, and a "paused" overtaken by the "playing" before it would
	/// leave the entry wrong until the next refresh.
	private var lastReport: Task<Void, Never>?

	/// The server drops an entry not heard from in five minutes, and a single
	/// track can outlast that. Well inside it, so one lost refresh is not
	/// enough to drop the entry.
	private static let refreshInterval: Duration = .seconds(60)
	/// A track change, a seek or a cast handover passes through a moment of
	/// not playing; reporting that as a pause would hide the user from others.
	private static let pauseDelay: Duration = .seconds(1)

	init(library: LibraryRepository) {
		self.library = library
	}

	/// Re-arms for the new track and announces it if it is already playing;
	/// otherwise `transport` does once it is.
	///
	/// Called with `nil` when the queue empties, so a later tick cannot submit
	/// a play for a track that is no longer current, and the server hears
	/// that nothing is.
	func trackChanged(to ref: ItemRef?) {
		let previous = current
		// The entry is one per account per server, so ending the queue, or
		// moving it to another server, has to end the one left behind.
		if let previous, previous.server != ref?.server, reported != .stopped {
			report(previous, .stopped, at: position)
		}
		current = ref
		submitted = false
		reported = nil
		position = 0
		cancelPause()
		// To *that track's* server. `LibraryRepository` resolves the client
		// from the ref, so nothing here closes over a current server - the same
		// property that makes a mixed-server queue play.
		if let ref, playing { report(ref, .starting, at: 0) }
	}

	/// The transport changed. `playing` should include buffering in order to
	/// play: a stall is not the user pausing.
	func transport(playing: Bool, position: Double) {
		self.playing = playing
		self.position = position
		guard let ref = current else { return }
		if playing {
			cancelPause()
			switch reported {
			case nil, .stopped: report(ref, .starting, at: position)
			case .paused: report(ref, .playing, at: position)
			case .starting, .playing: break
			}
		} else if reported != .paused, reported != .stopped, pendingPause == nil {
			pendingPause = Task { [weak self] in
				try? await Task.sleep(for: Self.pauseDelay)
				guard let self, !Task.isCancelled else { return }
				self.pendingPause = nil
				guard !self.playing, self.current == ref,
					self.reported != .paused, self.reported != .stopped
				else { return }
				self.report(ref, .paused, at: self.position)
			}
		}
	}

	/// The queue ran off its end: nothing is coming out of this device any
	/// more, although the last track stays current and can be replayed.
	func ended() {
		guard let ref = current, reported != .stopped else { return }
		cancelPause()
		report(ref, .stopped, at: position)
	}

	/// Driven from the periodic time observer, which only fires while the
	/// timeline is advancing - so this needs no "is it playing" guard of its
	/// own, unlike Android's, which polls on a timer of its own.
	func tick(position: Double, duration: Double) {
		self.position = position
		guard let ref = current else { return }
		if reported == .starting || reported == .playing,
			ContinuousClock.now - reportedAt >= Self.refreshInterval
		{
			report(ref, .playing, at: position)
		}
		guard !submitted else { return }
		guard ScrobbleRule.shouldSubmit(position: position, duration: duration) else { return }
		submitted = true
		Task { await library.scrobble(ref, submission: true) }
	}

	private func cancelPause() {
		pendingPause?.cancel()
		pendingPause = nil
	}

	private func report(_ ref: ItemRef, _ state: PlaybackState, at position: Double) {
		if ref == current {
			reported = state
			reportedAt = .now
		}
		let before = lastReport
		lastReport = Task { [library] in
			await before?.value
			await library.reportPlayback(ref, state: state, position: position)
		}
	}
}
