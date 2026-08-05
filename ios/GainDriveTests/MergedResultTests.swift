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

struct MergedResultTests {
	private func failure(_ message: String = "Nothing is listening at that address.")
		-> ServerFailure
	{
		ServerFailure(server: ServerId(), serverName: "Loft", message: message)
	}

	@Test func mapPreservesFailures() {
		let merged = MergedResult(items: [1, 2, 3], failures: [failure()])
		let mapped = merged.map { $0.count }
		#expect(mapped.items == 3)
		#expect(mapped.failures.count == 1)
		#expect(mapped.isPartial)
	}

	/// **Every server failing is a failed screen; some of them failing is a
	/// note over the ones that worked.** Expressed once, in `MergedResult.load`,
	/// so the browse view models cannot each get it slightly differently.
	@Test func everythingFailingIsAFailedScreen() {
		let merged = MergedResult(items: [Int](), failures: [failure("Cannot find that host.")])
		guard case .failed(let message) = merged.load else {
			Issue.record("expected a failed screen")
			return
		}
		#expect(message == "Cannot find that host.")
	}

	/// The property the whole type exists for: a screen must never be blank
	/// because the least important of three servers is down.
	@Test func someServersFailingStillShowsWhatArrived() {
		let merged = MergedResult(items: [1, 2], failures: [failure()])
		#expect(merged.load.value == [1, 2])
	}

	/// An empty library is not a failure. With no failures to report there is
	/// nothing to say, and "no artists" is the right screen.
	@Test func emptyWithoutFailuresIsReady() {
		let merged = MergedResult(items: [Int]())
		#expect(merged.load.value == [])
	}
}
