//	GainDrive for iOS
//	Copyright (C) 2026 Kasper Peeters
//
//	This file is part of GainDrive, distributed under the GNU General
//	Public License version 3 or later, with the Application Distribution
//	Exception in ios/LICENSE. See LICENSE at the repository root for the
//	full text of the GPL.

import Foundation

/// What is queued, where we are in it, and where the automatic tail begins.
///
/// **This is the truth; the player is reconciled to it.** `AVQueuePlayer`
/// cannot serve as the model - a played item is consumed and cannot be
/// re-enqueued, so it has no notion of "previous", and it cannot reorder.
///
/// A pure value type, so every piece of queue arithmetic in the phase is a
/// function with a unit test rather than behaviour buried in a class that needs
/// a player to exercise. That is what makes "pure functions are tested" cover
/// the risky code here rather than excuse it.
struct PlayQueue: Equatable, Sendable {
	private(set) var songs: [Song] = []
	private(set) var index = 0
	private(set) var autoFrom = QueueBoundary.empty

	var isEmpty: Bool { songs.isEmpty }

	var current: Song? {
		songs.indices.contains(index) ? songs[index] : nil
	}

	var hasNext: Bool { songs.indices.contains(index + 1) }
	var hasPrevious: Bool { index > 0 }

	/// The refs the engine should be holding: the current track, and as many
	/// after it as that engine wants.
	///
	/// **How many is the engine's business, not the queue's.** Locally it is
	/// two, because the second is what `AVQueuePlayer` pre-buffers and that
	/// pre-buffering is the whole of the gapless story available on this
	/// platform. A Cast receiver is told about **one** track at a time and the
	/// next is sent when it reports the first finished - which is what keeps
	/// this app the queue's owner, and what makes swapping engines mid-queue
	/// safe at all.
	func window(size: Int) -> [ItemRef] {
		guard size > 0, current != nil else { return [] }
		let end = Swift.min(index + size, songs.count)
		return songs[index..<end].map(\.ref)
	}

	/// The default two, for a caller that has no engine to ask - which in
	/// practice is the tests.
	var window: [ItemRef] { window(size: 2) }

	func song(for ref: ItemRef) -> Song? {
		songs.first { $0.ref == ref }
	}

	// MARK: - Mutations

	mutating func play(_ newSongs: [Song], startIndex: Int) {
		guard newSongs.indices.contains(startIndex) else { return }
		songs = newSongs
		index = startIndex
		autoFrom = autoFrom.afterPlay(startIndex: startIndex)
	}

	/// Drop the automatic tail, append, then move the boundary - **in that
	/// order**.
	///
	/// The truncation is the web client's rule and it is deliberate: without
	/// it, queueing a track behind a fifteen-track album buries it, which is
	/// never what "add to queue" is asked to mean.
	mutating func addToQueue(_ song: Song) {
		if let tail = autoFrom.tailToDrop(queueSize: songs.count) {
			songs.removeSubrange(tail)
			// The current track can only be inside a dropped tail if it was
			// itself automatic, in which case there is nothing left to play.
			index = min(index, max(songs.count - 1, 0))
		}
		songs.append(song)
		autoFrom = autoFrom.afterAppend(newCount: songs.count)
	}

	mutating func playNext(_ song: Song) {
		let at = songs.isEmpty ? 0 : index + 1
		songs.insert(song, at: at)
		autoFrom = autoFrom.afterInsert(at: at)
	}

	mutating func remove(at target: Int) {
		guard songs.indices.contains(target) else { return }
		songs.remove(at: target)
		autoFrom = autoFrom.afterRemove(at: target)
		if target < index { index -= 1 }
		index = min(index, max(songs.count - 1, 0))
	}

	/// A track the user dragged is a track the user chose, so the destination
	/// counts as hand-picked in both directions. That is a decision rather than
	/// a derivation - Android has no counterpart - and it is pinned by a test.
	mutating func move(from source: Int, to destination: Int) {
		guard songs.indices.contains(source), songs.indices.contains(destination),
			source != destination
		else { return }
		let moving = songs[index]
		let song = songs.remove(at: source)
		songs.insert(song, at: destination)
		autoFrom = autoFrom.afterRemove(at: source).afterInsert(at: destination)
		// The index follows the *song*, not the position: the track playing
		// must keep playing whatever moved around it.
		index = songs.firstIndex(of: moving) ?? index
	}

	mutating func jump(to target: Int) {
		guard songs.indices.contains(target) else { return }
		index = target
	}

	/// Advances by however many tracks the player got through, which is more
	/// than one only if it ran ahead while we were not looking.
	mutating func advance(by count: Int = 1) {
		index = min(index + count, max(songs.count - 1, 0))
	}

	mutating func goBack() {
		index = max(index - 1, 0)
	}

	/// Ran off the end. The queue is kept and the index parked on the last
	/// track, so the mini player does not vanish and the last track can be
	/// replayed.
	mutating func parkAtEnd() {
		index = max(songs.count - 1, 0)
	}

	mutating func clear() {
		songs = []
		index = 0
		autoFrom = .empty
	}
}
