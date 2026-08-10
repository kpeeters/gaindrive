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

/// Android's `QueueBoundaryTest.kt`, case for case. The failure mode is severe
/// enough to be worth the duplication: an off-by-one here means "add to queue"
/// quietly deletes tracks the user still wanted.
struct QueueBoundaryTests {
	@Test func playingAnAlbumMakesEverythingAfterTheTapAutomatic() {
		#expect(QueueBoundary.empty.afterPlay(startIndex: 2).value == 3)
		#expect(QueueBoundary.empty.afterPlay(startIndex: 0).value == 1)
	}

	@Test func tailToDropIsEverythingFromTheBoundary() {
		#expect(QueueBoundary(3).tailToDrop(queueSize: 10) == 3..<10)
	}

	/// `nil` and an empty range are not the same thing to a caller, and
	/// conflating them turns "drop from here" into "drop everything".
	@Test func thereIsNoTailWhenTheBoundaryIsTheEnd() {
		#expect(QueueBoundary(4).tailToDrop(queueSize: 4) == nil)
		#expect(QueueBoundary.adoptingExisting(queueSize: 7).tailToDrop(queueSize: 7) == nil)
	}

	@Test func appendingMovesTheBoundaryToTheEnd() {
		#expect(QueueBoundary(3).afterAppend(newCount: 5).value == 5)
	}

	/// The sequence that matters, and the one the whole type exists for.
	@Test func aHandPickedTrackSurvivesButTheAlbumTailDoesNot() {
		var boundary = QueueBoundary.empty.afterPlay(startIndex: 0)
		#expect(boundary.value == 1)

		// First enqueue: the nine automatic tracks behind the current one go.
		#expect(boundary.tailToDrop(queueSize: 10) == 1..<10)
		boundary = boundary.afterAppend(newCount: 2)
		#expect(boundary.value == 2)

		// Second enqueue: nothing to drop, because everything left was chosen.
		#expect(boundary.tailToDrop(queueSize: 2) == nil)
		boundary = boundary.afterAppend(newCount: 3)
		#expect(boundary.value == 3)
	}

	@Test func insertingAtOrBeforeTheBoundaryPushesIt() {
		#expect(QueueBoundary(1).afterInsert(at: 1).value == 2)
		#expect(QueueBoundary(5).afterInsert(at: 2).value == 6)
	}

	@Test func removingBeforeTheBoundaryPullsItBack() {
		#expect(QueueBoundary(3).afterRemove(at: 0).value == 2)
	}

	/// The boundary is the first *automatic* entry, so removing it is a removal
	/// from the tail rather than from the manual region.
	@Test func removingAtOrAfterTheBoundaryLeavesItAlone() {
		#expect(QueueBoundary(3).afterRemove(at: 7).value == 3)
		#expect(QueueBoundary(3).afterRemove(at: 3).value == 3)
	}

	/// A queue that outlived the process is treated as entirely hand-picked.
	@Test func anAdoptedQueueIsAllManual() {
		let adopted = QueueBoundary.adoptingExisting(queueSize: 12)
		#expect(adopted.value == 12)
		#expect(adopted.tailToDrop(queueSize: 12) == nil)
	}
}
