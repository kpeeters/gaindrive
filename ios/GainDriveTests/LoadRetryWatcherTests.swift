//	GainDrive for iOS
//	Copyright (C) 2026 Kasper Peeters
//
//	This file is part of GainDrive, distributed under the GNU General
//	Public License version 3 or later, with the Application Distribution
//	Exception in ios/LICENSE. See LICENSE at the repository root for the
//	full text of the GPL.

import Testing

@testable import GainDrive

/// The decision table behind re-sending a LOAD.
///
/// The behaviour it encodes was found by debugging a real receiver and is
/// described at length in the root `CLAUDE.md`; the point of testing it here is
/// that the rediscovery cost real time, and the msid filter in particular looks
/// like an optimisation right up until it is removed.
struct LoadRetryWatcherTests {
	private func status(
		_ state: CastPlayerState, msid: Int, idleReason: String? = nil
	) -> CastStatus {
		CastStatus(playerState: state, mediaSessionId: msid, idleReason: idleReason)
	}

	@Test func anUnarmedWatcherDecidesNothing() {
		var watcher = LoadRetryWatcher()
		#expect(!watcher.isArmed)
		#expect(!watcher.consume(status(.idle, msid: 9, idleReason: "ERROR")))
	}

	/// The failure this exists for: the new session goes to `IDLE`/`ERROR`
	/// without ever reaching `PLAYING`.
	@Test func anErrorOnTheNewSessionRetriesOnce() {
		var watcher = LoadRetryWatcher()
		watcher.arm(superseding: 4)
		#expect(watcher.consume(status(.idle, msid: 5, idleReason: "ERROR")))
		// Once, and only once — a second error is a different failure and must
		// not loop.
		#expect(!watcher.isArmed)
		#expect(!watcher.consume(status(.idle, msid: 5, idleReason: "ERROR")))
	}

	/// **The load-bearing case.** A `GET_STATUS` poll fired just before the
	/// receiver processed our LOAD comes back as a healthy `PLAYING` carrying
	/// the *old* session id. Treating it as evidence disarms the watcher, and
	/// the real error for the new session — which arrives later — is then
	/// ignored.
	@Test func aStalePlayingStatusDoesNotDisarm() {
		var watcher = LoadRetryWatcher()
		watcher.arm(superseding: 4)
		#expect(!watcher.consume(status(.playing, msid: 4)))
		#expect(watcher.isArmed)
		#expect(watcher.consume(status(.idle, msid: 5, idleReason: "ERROR")))
	}

	/// The intermediate `IDLE`/`INTERRUPTED` push carries the old id too, and is
	/// skipped for the same reason — which costs nothing, because the next push
	/// is the one that decides.
	@Test func aStaleInterruptedStatusIsIgnored() {
		var watcher = LoadRetryWatcher()
		watcher.arm(superseding: 4)
		#expect(!watcher.consume(status(.idle, msid: 4, idleReason: "INTERRUPTED")))
		#expect(watcher.isArmed)
	}

	@Test(arguments: [CastPlayerState.playing, .buffering, .loading])
	func aHealthyLoadDisarms(_ state: CastPlayerState) {
		var watcher = LoadRetryWatcher()
		watcher.arm(superseding: 4)
		#expect(!watcher.consume(status(state, msid: 5)))
		#expect(!watcher.isArmed)
		// And a later error is then somebody else's problem — a track that
		// played and then failed is not a load that never took.
		#expect(!watcher.consume(status(.idle, msid: 5, idleReason: "ERROR")))
	}

	/// `paused` decides nothing: it is neither the failure we retry nor proof
	/// the load took, and leaving the watcher armed costs nothing.
	@Test func pausedDecidesNothing() {
		var watcher = LoadRetryWatcher()
		watcher.arm(superseding: 4)
		#expect(!watcher.consume(status(.paused, msid: 5)))
		#expect(watcher.isArmed)
	}

	/// `FINISHED` is the queue advancing, not a load failing.
	@Test func aFinishedTrackIsNotARetry() {
		var watcher = LoadRetryWatcher()
		watcher.arm(superseding: 4)
		#expect(!watcher.consume(status(.idle, msid: 5, idleReason: "FINISHED")))
	}

	/// Nothing has played yet, so the superseded id is zero and every push
	/// carries a real one. This is the first LOAD of a session.
	@Test func theFirstLoadOfASessionSupersedesNothing() {
		var watcher = LoadRetryWatcher()
		watcher.arm(superseding: 0)
		#expect(watcher.consume(status(.idle, msid: 1, idleReason: "ERROR")))
	}

	@Test func disarmingStops() {
		var watcher = LoadRetryWatcher()
		watcher.arm(superseding: 4)
		watcher.disarm()
		#expect(!watcher.consume(status(.idle, msid: 5, idleReason: "ERROR")))
	}
}
