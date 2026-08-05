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

/// Composite identity. All of this is invisible with one server configured,
/// which is exactly why it is tested: the failure mode is tracks from the
/// wrong library the day a second server is added.
struct IdsTests {
	@Test func roundTripsThroughTheStringForm() throws {
		let ref = ItemRef(server: ServerId(), id: "1234")
		let decoded = try #require(ItemRef(encoded: ref.encoded))
		#expect(decoded == ref)
	}

	/// The property the whole design exists for.
	@Test func sameSubsonicIdOnTwoServersIsNotTheSameThing() {
		let one = ItemRef(server: ServerId(), id: "42")
		let two = ItemRef(server: ServerId(), id: "42")
		#expect(one != two)
		#expect(one.encoded != two.encoded)
		// And they must not collide as dictionary or Set keys either, which is
		// how a cache would confuse them.
		#expect(Set([one, two]).count == 2)
	}

	@Test func codableUsesTheSameEncoding() throws {
		let ref = ItemRef(server: ServerId(), id: "99")
		let data = try JSONEncoder().encode(ref)
		// A single string, not an object: Codable delegates to `encoded` so
		// there is only ever one serialisation to get wrong.
		#expect(String(data: data, encoding: .utf8) == "\"\(ref.encoded)\"")
		#expect(try JSONDecoder().decode(ItemRef.self, from: data) == ref)
	}

	/// Split on the *first* separator, so an id containing one survives.
	@Test func splitsOnTheFirstSeparatorOnly() throws {
		let server = ServerId()
		let ref = try #require(ItemRef(encoded: "\(server.value.uuidString)/a/b"))
		#expect(ref.server == server)
		#expect(ref.id == "a/b")
	}

	@Test(arguments: [
		"", "nothing", "not-a-uuid/42",
		"E621E1F8-C36C-495A-93FC-0C247A3E6E5F",
		"E621E1F8-C36C-495A-93FC-0C247A3E6E5F/",
	])
	func rejectsMalformedReferences(text: String) {
		#expect(ItemRef(encoded: text) == nil)
	}

	@Test func serverIdSurvivesJSON() throws {
		let id = ServerId()
		let data = try JSONEncoder().encode(id)
		#expect(try JSONDecoder().decode(ServerId.self, from: data) == id)
	}
}
