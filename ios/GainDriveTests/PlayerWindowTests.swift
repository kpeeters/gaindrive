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

/// The reconcile decision, tested without a player — which is the reason it was
/// extracted from one.
struct PlayerWindowTests {
	private let server = ServerId()

	private func ref(_ id: String) -> ItemRef {
		ItemRef(server: server, id: id)
	}

	@Test func anAlreadyMatchingWindowIsLeftAlone() {
		#expect(PlayerWindow.plan(current: [ref("1"), ref("2")], desired: [ref("1"), ref("2")]) == .none)
		#expect(PlayerWindow.plan(current: [], desired: []) == .none)
	}

	/// **The case that must not be got wrong.** Rebuilding here would remove
	/// and re-insert the head, restarting the track the user is listening to.
	@Test func onlyTheTailIsReplacedWhenOnlyTheTailChanged() {
		let edit = PlayerWindow.plan(
			current: [ref("1"), ref("2")], desired: [ref("1"), ref("9")])
		#expect(edit == .replaceTail([ref("9")]))
	}

	@Test func losingTheTailIsAlsoATailReplacement() {
		let edit = PlayerWindow.plan(current: [ref("1"), ref("2")], desired: [ref("1")])
		#expect(edit == .replaceTail([]))
	}

	@Test func gainingATailIsATailReplacement() {
		let edit = PlayerWindow.plan(current: [ref("1")], desired: [ref("1"), ref("2")])
		#expect(edit == .replaceTail([ref("2")]))
	}

	@Test func aDifferentHeadRebuilds() {
		let edit = PlayerWindow.plan(
			current: [ref("1"), ref("2")], desired: [ref("2"), ref("3")])
		#expect(edit == .rebuild([ref("2"), ref("3")]))
	}

	@Test func anEmptyPlayerRebuilds() {
		#expect(PlayerWindow.plan(current: [], desired: [ref("1")]) == .rebuild([ref("1")]))
	}

	@Test func anEmptyQueueClears() {
		#expect(PlayerWindow.plan(current: [ref("1")], desired: []) == .clear)
	}
}
