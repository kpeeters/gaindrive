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

/// The one test here that is not over a pure function, and it earns the
/// exception: the watchdog's whole job is three conditions checked after a
/// delay, none of which is visible by eye, and the thing it guards against is a
/// failure in which nothing else reports anything.
///
/// The timeout is an init parameter precisely so this can run in milliseconds.
@MainActor
struct PlaybackWatchdogTests {
	/// Stands in for `PlayerConnection`. A class rather than a captured `var`
	/// so the closures share one value and the test can move it under them.
	@MainActor
	private final class FakePlayer {
		var sample = PlaybackWatchdog.Sample(stalled: true, position: 12)
		var stalls = 0
	}

	private func makeWatchdog(_ fake: FakePlayer) -> PlaybackWatchdog {
		let watchdog = PlaybackWatchdog(timeout: .milliseconds(50))
		watchdog.sample = { fake.sample }
		watchdog.onStall = {
			fake.stalls += 1
			// What `PlayerConnection` does in response: pausing ends the stall,
			// so one wedge cannot be reported twice. Without it the counts
			// below would be "at least one" and would prove much less.
			fake.sample = PlaybackWatchdog.Sample(
				stalled: false, position: fake.sample.position)
		}
		return watchdog
	}

	@Test func firesWhenNothingHasMoved() async throws {
		let fake = FakePlayer()
		let watchdog = makeWatchdog(fake)
		watchdog.update()
		try await Task.sleep(for: .milliseconds(300))
		#expect(fake.stalls == 1)
	}

	/// The race the re-check exists for: the countdown and a recovery can
	/// overlap, and killing playback that has just started moving again would
	/// be the worse failure.
	@Test func doesNotFireWhenThePositionMovedOn() async throws {
		let fake = FakePlayer()
		let watchdog = makeWatchdog(fake)
		watchdog.update()
		fake.sample = PlaybackWatchdog.Sample(stalled: true, position: 14)
		try await Task.sleep(for: .milliseconds(300))
		#expect(fake.stalls == 0)
	}

	@Test func doesNotFireOnceTheStallIsOver() async throws {
		let fake = FakePlayer()
		let watchdog = makeWatchdog(fake)
		watchdog.update()
		fake.sample = PlaybackWatchdog.Sample(stalled: false, position: 12)
		watchdog.update()
		try await Task.sleep(for: .milliseconds(300))
		#expect(fake.stalls == 0)
	}

	/// **The one that would go unnoticed.** The transport publishes repeatedly
	/// through a stall, so a watchdog that re-armed on each call would push its
	/// own deadline out for ever and never fire at all.
	@Test func repeatedUpdatesDoNotPushTheDeadlineOut() async throws {
		let fake = FakePlayer()
		let watchdog = makeWatchdog(fake)
		for _ in 0..<5 {
			watchdog.update()
			try await Task.sleep(for: .milliseconds(20))
		}
		try await Task.sleep(for: .milliseconds(300))
		#expect(fake.stalls == 1)
	}

	/// A player that never stalls is never armed, so nothing to re-check and
	/// nothing to cancel.
	@Test func aHealthyPlayerIsNeverArmed() async throws {
		let fake = FakePlayer()
		fake.sample = PlaybackWatchdog.Sample(stalled: false, position: 0)
		let watchdog = makeWatchdog(fake)
		watchdog.update()
		try await Task.sleep(for: .milliseconds(300))
		#expect(fake.stalls == 0)
	}
}
