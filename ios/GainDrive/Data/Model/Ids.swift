//	GainDrive for iOS
//	Copyright (C) 2026 Kasper Peeters
//
//	This file is part of GainDrive, distributed under the GNU General
//	Public License version 3 or later, with the Application Distribution
//	Exception in ios/LICENSE. See LICENSE at the repository root for the
//	full text of the GPL.

import Foundation

/// A locally generated, stable identifier for a configured server.
///
/// **Not derived from the URL.** A server that moves from a LAN address to a
/// domain name is still the same server, and its ids, starred items and queue
/// references have to survive the move.
struct ServerId: Hashable, Sendable, Codable, CustomStringConvertible {
	let value: UUID

	init(_ value: UUID = UUID()) {
		self.value = value
	}

	var description: String { value.uuidString }

	init(from decoder: Decoder) throws {
		let container = try decoder.singleValueContainer()
		let text = try container.decode(String.self)
		guard let uuid = UUID(uuidString: text) else {
			throw DecodingError.dataCorruptedError(
				in: container, debugDescription: "not a UUID: \(text)")
		}
		value = uuid
	}

	func encode(to encoder: Encoder) throws {
		var container = encoder.singleValueContainer()
		try container.encode(value.uuidString)
	}
}

/// Every identifier that crosses a layer boundary.
///
/// Subsonic ids are only meaningful relative to the server that issued them —
/// two servers will both have an artist with id 42 — so a bare id string is
/// never enough. This is a value type, not a convention: bare ids must not
/// appear in domain models, view-model state or navigation paths. The rule is
/// enforceable by inspection. **If a function takes a `String` id and no
/// `ServerId`, it is wrong.**
///
/// Getting this wrong is not a bug that shows up while testing with one
/// server. It shows up as tracks from the wrong library the day a second one
/// is added, which is why the composite id exists from the very first call
/// rather than being retrofitted.
struct ItemRef: Hashable, Sendable {
	let server: ServerId
	let id: String

	init(server: ServerId, id: String) {
		self.server = server
		self.id = id
	}

	/// `<serverId>/<itemId>`, for cache keys, pin records and anything else
	/// persisted as a single string. Server ids are UUIDs and Subsonic ids are
	/// integers, so neither half can contain the separator.
	var encoded: String { "\(server.value.uuidString)/\(id)" }

	/// Split on the *first* separator only, so an id that somehow contains one
	/// survives the round trip rather than being silently truncated.
	init?(encoded: String) {
		guard let slash = encoded.firstIndex(of: "/") else { return nil }
		guard let uuid = UUID(uuidString: String(encoded[encoded.startIndex..<slash])) else {
			return nil
		}
		let rest = String(encoded[encoded.index(after: slash)...])
		guard !rest.isEmpty else { return nil }
		server = ServerId(uuid)
		id = rest
	}
}

/// Codable goes through the string form deliberately, so `encoded` and
/// `init?(encoded:)` remain the *only* place the encoding is known. A
/// synthesised conformance would be a second, silently divergent one.
extension ItemRef: Codable {
	init(from decoder: Decoder) throws {
		let container = try decoder.singleValueContainer()
		let text = try container.decode(String.self)
		guard let ref = ItemRef(encoded: text) else {
			throw DecodingError.dataCorruptedError(
				in: container, debugDescription: "not an item reference: \(text)")
		}
		self = ref
	}

	func encode(to encoder: Encoder) throws {
		var container = encoder.singleValueContainer()
		try container.encode(encoded)
	}
}
