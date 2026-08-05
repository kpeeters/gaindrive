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

/// The decoding wrappers, tested once so the DTOs do not have to be.
struct TolerantDecodingTests {
	private struct Holder: Decodable {
		@Listed var items: [Item] = []
		@Loose var id: String?
		@Loose var count: Int?
		@Loose var flag: Bool?

		struct Item: Decodable, Sendable, Equatable {
			let name: String
		}
	}

	private func decode(_ json: String) throws -> Holder {
		try JSONDecoder().decode(Holder.self, from: Data(json.utf8))
	}

	// MARK: - Lists

	@Test func decodesAnArray() throws {
		#expect(try decode(#"{"items":[{"name":"a"},{"name":"b"}]}"#).items.count == 2)
	}

	/// The single-element-collapsing trap: some servers emit a bare object
	/// where the schema says array when there is exactly one element.
	@Test func decodesABareObjectAsOneElement() throws {
		let holder = try decode(#"{"items":{"name":"only"}}"#)
		#expect(holder.items == [Holder.Item(name: "only")])
	}

	@Test func absentListIsEmpty() throws {
		#expect(try decode("{}").items.isEmpty)
	}

	@Test func nullListIsEmpty() throws {
		#expect(try decode(#"{"items":null}"#).items.isEmpty)
	}

	/// A genuinely malformed list is far likelier than a collapsed one, so its
	/// message must name the thing that is actually wrong rather than
	/// complaining that an array is not an object.
	@Test func malformedArrayRethrowsTheArraysError() {
		#expect(throws: DecodingError.self) {
			try decode(#"{"items":[{"wrong":1}]}"#)
		}
	}

	// MARK: - Scalars

	@Test(arguments: [
		(#""42""#, "42"), ("42", "42"), ("true", "true"),
	])
	func readsAStringFromAnyScalar(json: String, expected: String) throws {
		#expect(try decode(#"{"id":\#(json)}"#).id == expected)
	}

	/// The case that motivates the whole wrapper: a legacy server emitting an
	/// unquoted id. Without this the `typeMismatch` takes the entire response
	/// down, so one odd field costs the whole artist list.
	@Test func anUnquotedIdDoesNotFailTheResponse() throws {
		let holder = try decode(#"{"id":1,"items":[{"name":"a"}]}"#)
		#expect(holder.id == "1")
		#expect(holder.items.count == 1)
	}

	@Test(arguments: [("1999", 1999), (#""1999""#, 1999), ("1999.0", 1999)])
	func readsAnIntFromAnyScalar(json: String, expected: Int) throws {
		#expect(try decode(#"{"count":\#(json)}"#).count == expected)
	}

	@Test(arguments: ["true", "1", #""true""#, #""yes""#])
	func readsATrueFromAnyScalar(json: String) throws {
		#expect(try decode(#"{"flag":\#(json)}"#).flag == true)
	}

	/// The property the design exists for: a field the server typed wrongly
	/// becomes an absent field, and **the surrounding object still decodes**.
	@Test func aWronglyTypedScalarIsNilRatherThanFatal() throws {
		let holder = try decode(#"{"count":"nineteen","items":[{"name":"a"}]}"#)
		#expect(holder.count == nil)
		#expect(holder.items.count == 1)
	}

	@Test func absentAndNullScalarsAreNil() throws {
		#expect(try decode("{}").id == nil)
		#expect(try decode(#"{"id":null}"#).id == nil)
	}

	/// A number too large for `Int` must not trap. `Int(_:)` on a `Double`
	/// does; `Int(exactly:)` does not, which is why it is used.
	@Test func anOversizedNumberIsNilRatherThanACrash() throws {
		#expect(try decode(#"{"count":1e30}"#).count == nil)
	}
}
