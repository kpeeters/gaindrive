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

/// The envelope and the error mapping.
///
/// This is where the bugs are both likely and hard to see by eye: a mis-parsed
/// envelope looks like working code and fails silently on real data, whereas a
/// broken screen fails visibly the moment you open it.
struct SubsonicResponseTests {
	private func decode<Body: Decodable & Sendable>(
		_ json: String, expecting type: Body.Type, httpStatus: Int? = 200
	) throws -> Body {
		try SubsonicClient.decode(Data(json.utf8), expecting: type, httpStatus: httpStatus)
	}

	@Test func unwrapsSuccessfulEnvelope() throws {
		let body: UserBody = try decode(
			"""
			{"subsonic-response":{"status":"ok","version":"1.16.1",
			 "user":{"username":"admin","adminRole":true,"uploadRole":false,"maxBitRate":0}}}
			""", expecting: UserBody.self)
		#expect(body.user.username == "admin")
		#expect(body.user.isAdmin)
		#expect(body.user.canUpload == false)
	}

	@Test func pingCarriesNoBody() throws {
		_ = try decode(#"{"subsonic-response":{"status":"ok","version":"1.16.1"}}"#, expecting: EmptyBody.self)
	}

	/// Each documented code maps to its own case, so a caller can tell "wrong
	/// password" from "not allowed" without string matching.
	@Test(arguments: [
		(10, 10), (40, 40), (50, 50), (70, 70), (0, 0), (99, 99),
	])
	func mapsErrorCodes(sent: Int, expected: Int) throws {
		let json = """
			{"subsonic-response":{"status":"failed","version":"1.16.1",
			 "error":{"code":\(sent),"message":"nope"}}}
			"""
		let error = #expect(throws: SubsonicError.self) {
			try decode(json, expecting: UserBody.self)
		}
		#expect(error?.code == expected)
		#expect(error?.serverMessage == "nope")
	}

	@Test func distinguishesCredentialFailureFromRefusal() throws {
		let wrong = #expect(throws: SubsonicError.self) {
			try decode(
				#"{"subsonic-response":{"status":"failed","error":{"code":40}}}"#, expecting: UserBody.self)
		}
		#expect(wrong == SubsonicError.wrongCredentials(nil))

		let refused = #expect(throws: SubsonicError.self) {
			try decode(
				#"{"subsonic-response":{"status":"failed","error":{"code":50}}}"#, expecting: UserBody.self)
		}
		#expect(refused == SubsonicError.notAuthorised(nil))
	}

	/// A failed status with no error object. Real servers do this, and it must
	/// not surface as a decoding error.
	@Test func failedStatusWithoutErrorObject() throws {
		let error = #expect(throws: SubsonicError.self) {
			try decode(#"{"subsonic-response":{"status":"failed"}}"#, expecting: UserBody.self)
		}
		#expect(error == SubsonicError.failedWithoutError)
	}

	/// Almost every field beyond the identifier is optional, so a reply
	/// carrying only the mandatory one has to map without throwing.
	@Test func toleratesMinimalUser() throws {
		let body: UserBody = try decode(
			#"{"subsonic-response":{"status":"ok","user":{"username":"someone"}}}"#,
			expecting: UserBody.self)
		#expect(body.user.username == "someone")
		#expect(body.user.adminRole == nil)
		#expect(body.user.maxBitRate == nil)
		// The accessors decide the fallback once, rather than at every use.
		#expect(body.user.isAdmin == false)
	}

	/// An OpenSubsonic server adding a field must be a non-event.
	@Test func toleratesUnknownFields() throws {
		let body: UserBody = try decode(
			"""
			{"subsonic-response":{"status":"ok","serverVersion":"1.2.3","openSubsonic":true,
			 "user":{"username":"someone","futureRole":true,"nested":{"a":[1,2,3]}}}}
			""", expecting: UserBody.self)
		#expect(body.user.username == "someone")
	}

	/// Bandcamp's bridge answers a bad account with an empty 500. Reporting
	/// the status beats reporting the parse failure it caused.
	@Test func nonSuccessStatusWithNoEnvelope() throws {
		let error = #expect(throws: SubsonicError.self) {
			try decode("", expecting: UserBody.self, httpStatus: 500)
		}
		#expect(error == SubsonicError.httpStatus(500))
	}

	/// The same garbage on a 200 is a malformed reply, not an HTTP failure.
	@Test func garbageOnSuccessStatus() throws {
		let error = #expect(throws: SubsonicError.self) {
			try decode("not json at all", expecting: UserBody.self, httpStatus: 200)
		}
		#expect(error == SubsonicError.malformedResponse)
	}
}
