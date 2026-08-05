//	GainDrive for iOS
//	Copyright (C) 2026 Kasper Peeters
//
//	This file is part of GainDrive, distributed under the GNU General
//	Public License version 3 or later, with the Application Distribution
//	Exception in ios/LICENSE. See LICENSE at the repository root for the
//	full text of the GPL.

import Foundation

/// A Subsonic envelope whose `status` was not `ok`, mapped to a case per
/// documented code.
///
/// Nothing here takes the whole app out of service. With several servers, a
/// bad account or a missing feature is a condition of *one* server: 40 marks
/// that server as needing its credentials re-entered, 50 disables the offending
/// feature for it, 70 is an ordinary "not found". The other servers carry on.
enum SubsonicError: Error, Equatable, Sendable {
	/// 10 — a required parameter was missing.
	case missingParameter(String?)
	/// 40 — wrong username or password.
	case wrongCredentials(String?)
	/// 50 — the account is not authorised for this operation.
	case notAuthorised(String?)
	/// 70 — the requested data does not exist.
	case notFound(String?)
	/// Any other code the server chose to send.
	case server(code: Int, message: String?)
	/// `status` was not `ok` but no `error` object came with it. Real servers
	/// do this, and treating it as a decode failure would report a puzzling
	/// parse error instead of "the server refused".
	case failedWithoutError
	/// The envelope itself did not have the shape the API guarantees.
	case malformedResponse
	/// A non-2xx reply that carried no envelope to explain itself. Bandcamp's
	/// Subsonic bridge does this: its `ping` answers `ok` whatever credentials
	/// it is given, and a bad account instead surfaces as an empty 500 from
	/// the next endpoint. Without this case that arrives as a decoding error
	/// and reads like a parser bug.
	case httpStatus(Int)

	init(code: Int, message: String?) {
		switch code {
		case 10: self = .missingParameter(message)
		case 40: self = .wrongCredentials(message)
		case 50: self = .notAuthorised(message)
		case 70: self = .notFound(message)
		default: self = .server(code: code, message: message)
		}
	}

	/// The numeric code as the server sent it, for the cases that carry one.
	var code: Int? {
		switch self {
		case .missingParameter: 10
		case .wrongCredentials: 40
		case .notAuthorised: 50
		case .notFound: 70
		case .server(let code, _): code
		case .failedWithoutError, .malformedResponse, .httpStatus: nil
		}
	}

	/// The server's own wording, which is more specific than ours whenever it
	/// is present and is what a self-hoster needs in order to fix something.
	var serverMessage: String? {
		switch self {
		case .missingParameter(let m), .wrongCredentials(let m),
			.notAuthorised(let m), .notFound(let m), .server(_, let m):
			m
		case .failedWithoutError, .malformedResponse, .httpStatus:
			nil
		}
	}
}

extension SubsonicError: LocalizedError {
	var errorDescription: String? {
		if let serverMessage, !serverMessage.isEmpty { return serverMessage }
		switch self {
		case .missingParameter: return "The server rejected the request as incomplete."
		case .wrongCredentials: return "Wrong username or password."
		case .notAuthorised: return "This account is not allowed to do that."
		case .notFound: return "Not found on this server."
		case .server(let code, _): return "The server reported error \(code)."
		case .failedWithoutError: return "The server refused the request without saying why."
		case .malformedResponse: return "The server's reply was not in the expected format."
		case .httpStatus(let status): return "The server answered HTTP \(status) with no explanation."
		}
	}
}
