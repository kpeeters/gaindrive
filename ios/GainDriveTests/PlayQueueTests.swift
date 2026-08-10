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

/// The queue's arithmetic. This is the reason `PlayQueue` is a value type
/// rather than state inside `PlayerConnection`: the risky part of the phase is
/// testable without a player.
struct PlayQueueTests {
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

	private func titles(_ queue: PlayQueue) -> [String] {
		queue.songs.map(\.ref.id)
	}

	// MARK: - Playing

	@Test func playingSetsIndexAndBoundaryTogether() {
		var queue = PlayQueue()
		queue.play(album(10), startIndex: 2)
		#expect(queue.index == 2)
		#expect(queue.autoFrom.value == 3)
		#expect(queue.current?.ref.id == "2")
	}

	@Test func playingOutOfRangeChangesNothing() {
		var queue = PlayQueue()
		queue.play(album(3), startIndex: 9)
		#expect(queue.isEmpty)
	}

	// MARK: - The destructive one

	/// The order is load-bearing: drop the tail, append, *then* move the
	/// boundary. Any other order either keeps the tail or loses the new track.
	@Test func addToQueueDropsTheAutomaticTail() {
		var queue = PlayQueue()
		queue.play(album(10), startIndex: 0)
		queue.addToQueue(song("x"))

		#expect(titles(queue) == ["0", "x"])
		#expect(queue.autoFrom.value == 2)
		#expect(queue.index == 0)
	}

	/// The property the truncation exists for: a second enqueue must not bury
	/// the first behind anything, and must not delete it either.
	@Test func aHandPickedTrackSurvivesTheNextEnqueue() {
		var queue = PlayQueue()
		queue.play(album(10), startIndex: 0)
		queue.addToQueue(song("x"))
		queue.addToQueue(song("y"))

		#expect(titles(queue) == ["0", "x", "y"])
		#expect(queue.autoFrom.value == 3)
	}

	@Test func addToQueueOnAnEmptyQueueJustAppends() {
		var queue = PlayQueue()
		queue.addToQueue(song("x"))
		#expect(titles(queue) == ["x"])
		#expect(queue.current?.ref.id == "x")
	}

	// MARK: - Play next

	@Test func playNextLandsAfterTheCurrentTrackAndLeavesTheIndexAlone() {
		var queue = PlayQueue()
		queue.play(album(5), startIndex: 1)
		queue.playNext(song("x"))

		#expect(titles(queue) == ["0", "1", "x", "2", "3", "4"])
		#expect(queue.index == 1)
		#expect(queue.current?.ref.id == "1")
		#expect(queue.autoFrom.value == 3)
	}

	// MARK: - Removal

	@Test func removingBeforeTheCurrentTrackKeepsItPlaying() {
		var queue = PlayQueue()
		queue.play(album(5), startIndex: 3)
		queue.remove(at: 0)
		#expect(queue.index == 2)
		#expect(queue.current?.ref.id == "3")
	}

	@Test func removingAfterTheCurrentTrackChangesNothingElse() {
		var queue = PlayQueue()
		queue.play(album(5), startIndex: 1)
		queue.remove(at: 4)
		#expect(queue.index == 1)
		#expect(queue.current?.ref.id == "1")
	}

	@Test func removingTheCurrentTrackKeepsTheIndexInRange() {
		var queue = PlayQueue()
		queue.play(album(3), startIndex: 2)
		queue.remove(at: 2)
		#expect(queue.index == 1)
		#expect(queue.current?.ref.id == "1")
	}

	@Test func removingTheOnlyTrackEmptiesTheQueue() {
		var queue = PlayQueue()
		queue.play(album(1), startIndex: 0)
		queue.remove(at: 0)
		#expect(queue.isEmpty)
		#expect(queue.current == nil)
	}

	// MARK: - Moving

	/// The index follows the *song*, not the position: the track playing must
	/// keep playing whatever moved around it.
	@Test func movingCarriesTheCurrentTrackWithIt() {
		var queue = PlayQueue()
		queue.play(album(4), startIndex: 1)
		queue.move(from: 1, to: 3)

		#expect(titles(queue) == ["0", "2", "3", "1"])
		#expect(queue.current?.ref.id == "1")
		#expect(queue.index == 3)
	}

	@Test func movingAnotherTrackDoesNotDisturbTheCurrentOne() {
		var queue = PlayQueue()
		queue.play(album(4), startIndex: 1)
		queue.move(from: 3, to: 0)
		#expect(queue.current?.ref.id == "1")
		#expect(queue.index == 2)
	}

	// MARK: - Advancing

	@Test func advancingWalksTheQueue() {
		var queue = PlayQueue()
		queue.play(album(3), startIndex: 0)
		queue.advance()
		#expect(queue.index == 1)
		queue.advance(by: 1)
		#expect(queue.index == 2)
		#expect(!queue.hasNext)
	}

	/// Running off the end parks on the last track rather than clearing, so the
	/// mini player does not vanish and the track can be replayed.
	@Test func parkingAtTheEndKeepsTheQueue() {
		var queue = PlayQueue()
		queue.play(album(3), startIndex: 0)
		queue.parkAtEnd()
		#expect(queue.index == 2)
		#expect(queue.current?.ref.id == "2")
		#expect(!queue.isEmpty)
	}

	// MARK: - The window

	@Test func theWindowIsTheCurrentTrackAndTheNext() {
		var queue = PlayQueue()
		queue.play(album(5), startIndex: 1)
		#expect(queue.window.map(\.id) == ["1", "2"])
	}

	@Test func theWindowIsOneTrackAtTheEnd() {
		var queue = PlayQueue()
		queue.play(album(2), startIndex: 1)
		#expect(queue.window.map(\.id) == ["1"])
	}

	@Test func theWindowIsEmptyWithNoQueue() {
		#expect(PlayQueue().window.isEmpty)
	}
}
