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

/// The reconcile rule is what turns a star the server silently ignored back
/// into an outline, and what keeps one it did take from flipping back.
@MainActor
struct StarStoreTests {
	private let a = ServerId()
	private let b = ServerId()

	@Test func anAgreedOverrideIsKept() {
		let ref = ItemRef(server: a, id: "1")
		let result = StarStore.reconciled([ref: true], server: a, truth: [ref])
		#expect(result == [ref: true])
	}

	@Test func aStarTheServerDidNotRecordReverts() {
		let ref = ItemRef(server: a, id: "1")
		let result = StarStore.reconciled([ref: true], server: a, truth: [])
		#expect(result == [ref: false])
	}

	@Test func anUnstarTheServerDidNotRecordReverts() {
		let ref = ItemRef(server: a, id: "1")
		let result = StarStore.reconciled([ref: false], server: a, truth: [ref])
		#expect(result == [ref: true])
	}

	@Test func otherServersAreLeftAlone() {
		let onA = ItemRef(server: a, id: "1")
		let onB = ItemRef(server: b, id: "1")
		let result = StarStore.reconciled([onA: true, onB: true], server: a, truth: [])
		#expect(result == [onA: false, onB: true])
	}
}
