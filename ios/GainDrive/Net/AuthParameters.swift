//	GainDrive for iOS
//	Copyright (C) 2026 Kasper Peeters
//
//	This file is part of GainDrive, distributed under the GNU General
//	Public License version 3 or later, with the Application Distribution
//	Exception in ios/LICENSE. See LICENSE at the repository root for the
//	full text of the GPL.

import CryptoKit
import Foundation

/// The one place credentials become query parameters.
///
/// Every request the app makes - API calls, cover-art URLs, stream URLs handed
/// to `AVPlayer` - is built through this type, so credentials cannot leak from
/// one server's request to another's by construction rather than by
/// discipline. Android achieves the same thing with a per-server OkHttp
/// interceptor; here it is a value each `SubsonicClient` holds.
///
/// The token scheme rather than the plaintext `p=` the web client sends:
/// request URLs end up in logs, in image-loader cache keys and in the media
/// stack, and an MD5 the server already accepts is better in all of those than
/// the password itself.
struct AuthParameters: Sendable {
	static let apiVersion = "1.16.1"
	static let clientName = "gaindrive-ios"

	let username: String
	private let password: String

	/// Generated once per client, which means once per server per app session
	/// - deliberately *not* per request. A per-request salt gives every
	/// cover-art URL a unique query string, so a URL-keyed image cache misses
	/// every single time and re-downloads every thumbnail on every scroll.
	let salt: String

	/// Both credentials are trimmed. Bandcamp issues a 32-character token as
	/// the username, which is pasted, which is exactly how a stray space gets
	/// in; it would then travel as `%20` on every request, and a server whose
	/// `ping` does not authenticate accepts the account and fails later on the
	/// first endpoint that looks the user up. Trimming the password discards a
	/// theoretically valid one - that is the accepted trade, and the same one
	/// Android made.
	init(username: String, password: String, salt: String = AuthParameters.makeSalt()) {
		self.username = username.trimmingCharacters(in: .whitespacesAndNewlines)
		self.password = password.trimmingCharacters(in: .whitespacesAndNewlines)
		self.salt = salt
	}

	/// `md5(password + salt)`, lowercase hex. The server recomputes exactly
	/// this in `MediaStore::validate_auth()` and compares case-insensitively.
	var token: String {
		Insecure.MD5.hash(data: Data((password + salt).utf8))
			.map { String(format: "%02x", $0) }
			.joined()
	}

	/// Note what is *not* here: `p`. A test asserts on that, because the whole
	/// point of the token scheme is defeated the moment a plaintext password
	/// reaches a URL.
	var queryItems: [URLQueryItem] {
		[
			URLQueryItem(name: "u", value: username),
			URLQueryItem(name: "t", value: token),
			URLQueryItem(name: "s", value: salt),
			URLQueryItem(name: "v", value: Self.apiVersion),
			URLQueryItem(name: "c", value: Self.clientName),
			URLQueryItem(name: "f", value: "json"),
		]
	}

	static func makeSalt() -> String {
		(0..<8).map { _ in String(format: "%02x", UInt8.random(in: .min ... .max)) }.joined()
	}
}
