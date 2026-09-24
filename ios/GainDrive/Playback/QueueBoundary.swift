//	GainDrive for iOS
//	Copyright (C) 2026 Kasper Peeters
//
//	This file is part of GainDrive, distributed under the GNU General
//	Public License version 3 or later, with the Application Distribution
//	Exception in ios/LICENSE. See LICENSE at the repository root for the
//	full text of the GPL.

import Foundation

/// Where the hand-picked part of the queue ends and the automatic tail begins.
///
/// A verbatim port of `playback/QueueBoundary.kt`, and pure arithmetic on
/// purpose: these are one-line rules whose failure mode is severe - an
/// off-by-one in `afterAppend` or `tailToDrop` means "add to queue" quietly
/// deletes tracks the user still wanted.
///
/// `value` is the index of the first *automatic* entry.
struct QueueBoundary: Hashable, Sendable {
	let value: Int

	init(_ value: Int) {
		self.value = value
	}

	static let empty = QueueBoundary(0)

	/// Playing an album makes everything after the tapped track automatic.
	func afterPlay(startIndex: Int) -> QueueBoundary {
		QueueBoundary(startIndex + 1)
	}

	/// An appended track is hand-picked, so the boundary moves to the end.
	func afterAppend(newCount: Int) -> QueueBoundary {
		QueueBoundary(newCount)
	}

	func afterInsert(at index: Int) -> QueueBoundary {
		value <= index ? QueueBoundary(index + 1) : QueueBoundary(value + 1)
	}

	/// Removing *at* the boundary is a removal from the tail, not from the
	/// manual region - the boundary is the first automatic entry, so it is
	/// itself automatic.
	func afterRemove(at index: Int) -> QueueBoundary {
		index < value ? QueueBoundary(value - 1) : self
	}

	/// The automatic tail, or `nil` when there is none.
	///
	/// `nil` and an empty range are not the same thing to a caller, and
	/// conflating them is how a removal of "everything from here" becomes a
	/// removal of everything.
	func tailToDrop(queueSize: Int) -> Range<Int>? {
		value < queueSize ? value..<queueSize : nil
	}

	/// A queue that outlived the process is treated as **entirely
	/// hand-picked**. Assuming the opposite would let the next enqueue delete a
	/// queue the user still wanted, which is much worse than keeping too much.
	static func adoptingExisting(queueSize: Int) -> QueueBoundary {
		QueueBoundary(queueSize)
	}
}
