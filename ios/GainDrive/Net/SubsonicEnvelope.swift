//	GainDrive for iOS
//	Copyright (C) 2026 Kasper Peeters
//
//	This file is part of GainDrive, distributed under the GNU General
//	Public License version 3 or later, with the Application Distribution
//	Exception in ios/LICENSE. See LICENSE at the repository root for the
//	full text of the GPL.

import Foundation

/// The `{"subsonic-response": {...}}` wrapper every endpoint replies with.
///
/// The payload is a *sibling* of `status` inside the wrapper rather than
/// nested under a key of its own, so `Body` is decoded from the same container
/// — which is why this has a hand-written `init(from:)` instead of the
/// synthesised one.
struct SubsonicEnvelope<Body: Decodable & Sendable>: Decodable, Sendable {
	let status: String
	let version: String?
	let error: ErrorBody?
	let body: Body?

	struct ErrorBody: Decodable, Sendable, Equatable {
		let code: Int
		let message: String?
	}

	private enum Outer: String, CodingKey {
		case subsonicResponse = "subsonic-response"
	}

	private enum Inner: String, CodingKey {
		case status, version, error
	}

	init(from decoder: Decoder) throws {
		let outer = try decoder.container(keyedBy: Outer.self)
		let inner = try outer.nestedContainer(keyedBy: Inner.self, forKey: .subsonicResponse)
		// A reply with no status at all is a failure, not a crash: treating it
		// as `failed` routes it through the same path as an explicit refusal.
		status = try inner.decodeIfPresent(String.self, forKey: .status) ?? "failed"
		version = try inner.decodeIfPresent(String.self, forKey: .version)
		error = try inner.decodeIfPresent(ErrorBody.self, forKey: .error)
		// Only attempted on success. A failed envelope has no payload, and
		// trying to decode one would turn a clean "wrong password" into a
		// decoding error that says nothing useful.
		if status == "ok" {
			body = try Body(from: outer.superDecoder(forKey: .subsonicResponse))
		} else {
			body = nil
		}
	}

	/// The payload, or the failure the envelope described.
	func unwrap() throws -> Body {
		guard status == "ok" else {
			guard let error else { throw SubsonicError.failedWithoutError }
			throw SubsonicError(code: error.code, message: error.message)
		}
		guard let body else { throw SubsonicError.malformedResponse }
		return body
	}
}

/// For endpoints whose success carries nothing but the status — `ping` being
/// the one phase 1 uses. An empty struct decodes from any object.
struct EmptyBody: Decodable, Sendable {}

/// `getUser`. Only the fields the app acts on are modelled; unknown ones are
/// ignored, which is what makes an OpenSubsonic server adding fields a
/// non-event.
///
/// Every field but `username` is optional. Almost everything in Subsonic's
/// JSON beyond an id is, and a response carrying only the mandatory fields has
/// to map without throwing — the alternative is an app that works against
/// gaindrive and crashes against something else.
struct SubsonicUser: Decodable, Sendable, Equatable {
	let username: String
	let adminRole: Bool?
	let uploadRole: Bool?
	let castRole: Bool?
	let downloadRole: Bool?
	let disabled: Bool?
	let maxBitRate: Int?

	/// Roles are per server: the same person can be an admin on one and a
	/// restricted account on another, so any UI gated on a role is gated on
	/// the role for the server that owns the item in question.
	var isAdmin: Bool { adminRole ?? false }
	var canUpload: Bool { uploadRole ?? false }
}

struct UserBody: Decodable, Sendable {
	let user: SubsonicUser
}
