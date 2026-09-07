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

/// When a play is submitted.
///
/// Worth pinning because both halves of the rule fail quietly in opposite
/// directions: submit too early and the server's play counts are wrong in a way
/// nobody notices, submit never and `getRecentSongs` stays empty in all three
/// clients.
struct ScrobbleRuleTests {
	@Test func aShortTrackCountsAtItsHalfwayMark() {
		#expect(!ScrobbleRule.shouldSubmit(position: 89, duration: 180))
		#expect(ScrobbleRule.shouldSubmit(position: 90, duration: 180))
	}

	/// The four-minute cap is what stops a long track having to be listened to
	/// halfway before it counts — an hour-long DJ set would otherwise need
	/// thirty minutes.
	@Test func aLongTrackCountsAfterFourMinutes() {
		let hour: Double = 3600
		#expect(!ScrobbleRule.shouldSubmit(position: 239, duration: hour))
		#expect(ScrobbleRule.shouldSubmit(position: 240, duration: hour))
	}

	/// The trap the guard exists for: every position is past half of nothing,
	/// so an unknown duration would submit on the very first tick of every
	/// track.
	@Test func anUnknownDurationSubmitsNothing() {
		#expect(!ScrobbleRule.shouldSubmit(position: 300, duration: 0))
		#expect(!ScrobbleRule.shouldSubmit(position: 300, duration: -1))
	}

	/// A position of zero is where every track starts, and starting a track is
	/// not listening to it — which matters for a zero-length row, where the
	/// halfway mark is also zero.
	@Test func theStartOfATrackIsNotAPlay() {
		#expect(!ScrobbleRule.shouldSubmit(position: 0, duration: 180))
	}

	@Test func aNonFinitePositionSubmitsNothing() {
		#expect(!ScrobbleRule.shouldSubmit(position: .nan, duration: 180))
		#expect(!ScrobbleRule.shouldSubmit(position: .infinity, duration: 180))
	}
}
