//	GainDrive for iOS
//	Copyright (C) 2026 Kasper Peeters
//
//	This file is part of GainDrive, distributed under the GNU General
//	Public License version 3 or later, with the Application Distribution
//	Exception in ios/LICENSE. See LICENSE at the repository root for the
//	full text of the GPL.

import Foundation
import Testing

@testable import GainDrive

/// The arithmetic between SwiftUI's drag gesture and `PlayQueue.move`.
///
/// `PlayQueueTests` covers the model; this covers the translation on top of it,
/// which is a separate off-by-one and fails in a way that reads as a laggy
/// gesture rather than as a bug — the row simply lands one place short of where
/// it was let go.
struct QueueListTests {
	private let server = ServerId()

	private func song(_ id: String) -> Song {
		Song(
			ref: ItemRef(server: server, id: id), title: "Track \(id)", artistName: "A",
			albumTitle: "B", albumRef: nil, track: nil, discNumber: nil, year: nil,
			duration: 100, bitRate: nil, suffix: nil, contentType: nil, sizeBytes: 0,
			coverArt: nil, starredAt: nil, lastPlayedAt: nil, isVideo: false,
			nativeSeek: false, width: nil, height: nil)
	}

	private func album(_ count: Int) -> [Song] {
		(0..<count).map { song(String($0)) }
	}

	/// Dropped **below** where it started: the offset counts the row itself,
	/// so the index it ends up at is one less.
	@Test func movingDownLosesOneToTheRowItself() {
		#expect(QueueMove.destination(from: 0, insertingBefore: 3) == 2)
		#expect(QueueMove.destination(from: 1, insertingBefore: 5) == 4)
	}

	/// Dropped **above**: nothing has been removed from in front of it, so the
	/// offset is already the index.
	@Test func movingUpTakesTheOffsetUnchanged() {
		#expect(QueueMove.destination(from: 4, insertingBefore: 1) == 1)
		#expect(QueueMove.destination(from: 4, insertingBefore: 0) == 0)
	}

	/// The degenerate case, which `PlayQueue.move` refuses anyway — but it must
	/// be refused rather than turned into a move by one.
	@Test func droppingARowWhereItAlreadyIsGoesNowhere() {
		#expect(QueueMove.destination(from: 2, insertingBefore: 2) == 2)
	}

	// MARK: - Composed with the model

	/// The property that matters, stated the way a user would: the track ends
	/// up where it was dropped.
	@Test func draggingTheFirstTrackToTheEndPutsItLast() {
		var queue = PlayQueue()
		queue.play(album(5), startIndex: 0)
		queue.move(from: 0, to: QueueMove.destination(from: 0, insertingBefore: 5))
		#expect(queue.songs.map(\.ref.id) == ["1", "2", "3", "4", "0"])
	}

	@Test func draggingALaterTrackUpPutsItThere() {
		var queue = PlayQueue()
		queue.play(album(5), startIndex: 0)
		queue.move(from: 3, to: QueueMove.destination(from: 3, insertingBefore: 1))
		#expect(queue.songs.map(\.ref.id) == ["0", "3", "1", "2", "4"])
	}

	// MARK: - Where the divider goes

	/// `QueueList` draws its "Continuing from the album" caption on the row at
	/// `autoFrom`, so where that lands *is* what the reader sees. Pinned here
	/// because the caption is a promise about what "add to queue" will replace.
	@Test func theDividerSitsOnTheFirstAutomaticTrack() {
		var queue = PlayQueue()
		queue.play(album(6), startIndex: 1)
		// Played from track 1, so 0 and 1 are behind the boundary and the
		// caption belongs on track 2.
		#expect(queue.autoFrom.value == 2)

		queue.addToQueue(song("x"))
		// The tail went, so every remaining track is hand-picked and there is
		// no caption to draw at all.
		#expect(queue.autoFrom.value == queue.songs.count)
		#expect(queue.songs.map(\.ref.id) == ["0", "1", "x"])
	}

	/// A caption on row 0 would be captioning the whole queue, which says
	/// nothing — hence the `index > 0` guard in `QueueList.row`.
	@Test func aQueueThatIsEntirelyAutomaticHasNoDividerToDraw() {
		var queue = PlayQueue()
		queue.play(album(4), startIndex: 0)
		queue.remove(at: 0)
		#expect(queue.autoFrom.value == 0)
	}
}
