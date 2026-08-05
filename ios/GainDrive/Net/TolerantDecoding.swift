//	GainDrive for iOS
//	Copyright (C) 2026 Kasper Peeters
//
//	This file is part of GainDrive, distributed under the GNU General
//	Public License version 3 or later, with the Application Distribution
//	Exception in ios/LICENSE. See LICENSE at the repository root for the
//	full text of the GPL.

import Foundation

/// The two decoding traps in Subsonic's JSON, solved once so no DTO has to
/// think about either.
///
/// Android gets four tolerances free from `kotlinx.serialization`:
/// `ignoreUnknownKeys`, `coerceInputValues`, `isLenient`, and a default on
/// every field. Swift's `Codable` gives exactly one of them — a keyed container
/// ignores unknown keys — and throws on the rest. These two wrappers cover the
/// gap, and nothing else is needed.

/// A list-valued field.
///
/// Tolerates three shapes the schema does not promise: the key being absent,
/// an explicit `null`, and **a bare object where the schema says array**. The
/// last is the single-element-collapsing trap: some Subsonic servers emit
/// `"artist": {…}` rather than `"artist": [{…}]` when there is exactly one.
/// gaindrive is consistent about it, but the tolerance costs one `catch` and
/// removes a whole class of crash against everything else.
@propertyWrapper
struct Listed<Element: Decodable & Sendable>: Decodable, Sendable {
	var wrappedValue: [Element]

	init(wrappedValue: [Element] = []) {
		self.wrappedValue = wrappedValue
	}

	init(from decoder: any Decoder) throws {
		let container = try decoder.singleValueContainer()
		if container.decodeNil() {
			wrappedValue = []
			return
		}
		do {
			wrappedValue = try container.decode([Element].self)
		} catch {
			// The *array*'s error is rethrown, not the element's. A genuinely
			// malformed list is far likelier than a collapsed one, and its
			// message names the thing that is actually wrong.
			guard let single = try? container.decode(Element.self) else { throw error }
			wrappedValue = [single]
		}
	}
}

/// A scalar whose JSON type the server may not have got right.
///
/// This is the half Android never had to write. `isLenient` reads an unquoted
/// `42` into a `String` field and `coerceInputValues` turns an explicit `null`
/// into the declared default; Swift has neither, so an id emitted as a number
/// throws `typeMismatch` and takes **the entire response** down with it — one
/// odd field costs the whole artist list. That is not hypothetical: enabling a
/// legacy server is exactly how Android discovered it needed `isLenient`.
///
/// Never throws, by design. A field the server typed wrongly becomes an absent
/// field, which the mapper already knows how to survive.
@propertyWrapper
struct Loose<Value: LooseScalar>: Decodable, Sendable {
	var wrappedValue: Value?

	init(wrappedValue: Value? = nil) {
		self.wrappedValue = wrappedValue
	}

	init(from decoder: any Decoder) throws {
		guard let container = try? decoder.singleValueContainer() else {
			wrappedValue = nil
			return
		}
		wrappedValue = Value(loose: container)
	}
}

/// A scalar that can be read from whichever JSON type it arrived as.
protocol LooseScalar: Sendable, Equatable {
	init?(loose container: any SingleValueDecodingContainer)
}

extension String: LooseScalar {
	init?(loose container: any SingleValueDecodingContainer) {
		if let value = try? container.decode(String.self) { self = value; return }
		if let value = try? container.decode(Int.self) { self = String(value); return }
		if let value = try? container.decode(Double.self) { self = String(value); return }
		if let value = try? container.decode(Bool.self) { self = String(value); return }
		return nil
	}
}

extension Int: LooseScalar {
	init?(loose container: any SingleValueDecodingContainer) {
		if let value = try? container.decode(Int.self) { self = value; return }
		// `Int(exactly:)` rather than `Int(_:)`: the latter traps on a value
		// too large to represent, and a hostile or broken server must not be
		// able to crash the app with a number.
		if let value = try? container.decode(Double.self),
			let exact = Int(exactly: value.rounded()) {
			self = exact
			return
		}
		if let text = try? container.decode(String.self), let value = Int(text) {
			self = value
			return
		}
		return nil
	}
}

extension Bool: LooseScalar {
	init?(loose container: any SingleValueDecodingContainer) {
		if let value = try? container.decode(Bool.self) { self = value; return }
		if let value = try? container.decode(Int.self) { self = value != 0; return }
		if let text = try? container.decode(String.self) {
			switch text.lowercased() {
			case "true", "1", "yes": self = true
			case "false", "0", "no": self = false
			default: return nil
			}
			return
		}
		return nil
	}
}

extension KeyedDecodingContainer {
	/// The synthesised `Decodable` calls `decode(_:forKey:)` for a wrapped
	/// property **even when the key is absent**, so a wrapper has no way to
	/// supply its own default without this. Chosen by the compiler over the
	/// generic `decode` because it is the more specialised overload.
	func decode<Element>(
		_ type: Listed<Element>.Type, forKey key: Key
	) throws -> Listed<Element> {
		try decodeIfPresent(type, forKey: key) ?? Listed()
	}

	func decode<Value>(
		_ type: Loose<Value>.Type, forKey key: Key
	) throws -> Loose<Value> {
		try decodeIfPresent(type, forKey: key) ?? Loose()
	}
}
