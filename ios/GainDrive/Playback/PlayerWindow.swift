//	GainDrive for iOS
//	Copyright (C) 2026 Kasper Peeters
//
//	This file is part of GainDrive, distributed under the GNU General
//	Public License version 3 or later, with the Application Distribution
//	Exception in ios/LICENSE. See LICENSE at the repository root for the
//	full text of the GPL.

import Foundation

/// What to do to the player's item list to make it match the queue.
enum WindowEdit: Equatable {
	case none
	/// Keep the current item — and its buffer, and its playback position —
	/// and replace what follows it.
	case replaceTail([ItemRef])
	case rebuild([ItemRef])
	case clear
}

/// Decides the edit. Pure, so the decision can be tested without a player.
///
/// The distinction that matters is between `replaceTail` and `rebuild`.
/// Rebuilding on every queue edit would remove and re-insert the head, which
/// **restarts the track the user is listening to** — the kind of bug that is
/// obvious the moment it happens and invisible in code review.
enum PlayerWindow {
	static func plan(current: [ItemRef], desired: [ItemRef]) -> WindowEdit {
		guard !desired.isEmpty else {
			return current.isEmpty ? .none : .clear
		}
		guard current.first == desired.first else {
			return .rebuild(desired)
		}
		let currentTail = Array(current.dropFirst())
		let desiredTail = Array(desired.dropFirst())
		return currentTail == desiredTail ? .none : .replaceTail(desiredTail)
	}
}
