//	GainDrive for iOS
//	Copyright (C) 2026 Kasper Peeters
//
//	This file is part of GainDrive, distributed under the GNU General
//	Public License version 3 or later, with the Application Distribution
//	Exception in ios/LICENSE. See LICENSE at the repository root for the
//	full text of the GPL.

import CryptoKit
import Foundation
import Testing

@testable import GainDrive

/// The test that proves the security property, so it asserts on the outgoing
/// URL rather than on an object.
///
/// `PLAN.md` expected this to need `URLProtocol` stubbing or a loopback
/// server. It does not, and the reason is a real difference from Android: there
/// the parameters are added by an OkHttp interceptor, which can only be
/// observed by making a request. Here `SubsonicClient.url` is a pure function
/// that *every* request, cover-art URL and stream URL is built from, so its
/// output is the outgoing URL. Asserting on it directly is the same guarantee
/// with none of the machinery.
struct AuthParametersTests {
	private let base = URL(string: "http://192.0.2.9:4040")!

	private func client(
		username: String = "admin", password: String = "secret", salt: String? = nil
	) -> SubsonicClient {
		let auth =
			salt.map { AuthParameters(username: username, password: password, salt: $0) }
			?? AuthParameters(username: username, password: password)
		return SubsonicClient(serverId: ServerId(), baseURL: base, auth: auth)
	}

	private func query(_ url: URL) -> [String: String] {
		let items = URLComponents(url: url, resolvingAgainstBaseURL: false)?.queryItems ?? []
		return Dictionary(items.compactMap { item in item.value.map { (item.name, $0) } }) { a, _ in a }
	}

	@Test func sendsTheTokenScheme() {
		let url = client(salt: "abcdef01").url("ping")
		let parameters = query(url)
		#expect(parameters["u"] == "admin")
		#expect(parameters["s"] == "abcdef01")
		#expect(parameters["v"] == "1.16.1")
		#expect(parameters["c"] == "gaindrive-ios")
		#expect(parameters["f"] == "json")
		#expect(parameters["t"]?.isEmpty == false)
	}

	/// The whole point of the token scheme is defeated the moment a plaintext
	/// password reaches a URL — where it would land in logs, in image-cache
	/// keys and in the media stack.
	@Test func neverSendsThePlaintextPassword() {
		let url = client(password: "hunter2").url("getUser", parameters: ["username": "admin"])
		#expect(query(url)["p"] == nil)
		#expect(!url.absoluteString.contains("hunter2"))
	}

	/// The value the server recomputes in `MediaStore::validate_auth()`.
	@Test func tokenIsMD5OfPasswordAndSalt() {
		let expected = Insecure.MD5.hash(data: Data("secretc19a4f".utf8))
			.map { String(format: "%02x", $0) }.joined()
		#expect(query(client(salt: "c19a4f").url("ping"))["t"] == expected)
	}

	/// Stable across requests from one client, because a per-request salt
	/// gives every cover-art URL a unique query string and misses a URL-keyed
	/// image cache every single time.
	@Test func saltIsStableWithinAClient() {
		let one = client()
		#expect(query(one.url("ping"))["s"] == query(one.url("getUser"))["s"])
	}

	/// And different between clients, so two servers never share a token.
	@Test func saltDiffersBetweenClients() {
		#expect(query(client().url("ping"))["s"] != query(client().url("ping"))["s"])
	}

	/// Bandcamp issues a pasted 32-character token as the username, which is
	/// exactly how a stray space gets in; untrimmed it travels as `%20` and
	/// fails on the first endpoint that looks the account up.
	@Test func trimsCredentials() {
		let padded = client(username: "  admin\n", password: " secret ", salt: "00")
		let clean = client(username: "admin", password: "secret", salt: "00")
		#expect(query(padded.url("ping"))["u"] == "admin")
		#expect(query(padded.url("ping"))["t"] == query(clean.url("ping"))["t"])
	}

	/// `URLComponents` leaves `+` alone because it is legal in a query string,
	/// but a form decoder reads it as a space — so `me+music@example.com`
	/// would arrive as `me music@example.com`.
	@Test func escapesPlusInValues() {
		let url = client(username: "me+music@example.com").url("ping")
		#expect(url.absoluteString.contains("%2B"))
		#expect(query(url)["u"] == "me+music@example.com")
	}

	@Test func buildsTheEndpointPath() {
		#expect(client().url("ping").path() == "/rest/ping.view")
		// The suffix exists for hls.m3u8, the one endpoint not spelled .view.
		#expect(client().url("hls", suffix: ".m3u8").path() == "/rest/hls.m3u8")
	}

	/// A base URL with a path of its own — a server behind a reverse proxy at
	/// /music — must keep it.
	@Test func preservesABasePath() {
		let nested = SubsonicClient(
			serverId: ServerId(),
			baseURL: URL(string: "https://example.com/music")!,
			auth: AuthParameters(username: "a", password: "b")
		)
		#expect(nested.url("ping").path() == "/music/rest/ping.view")
	}

	/// Same logical request, same URL — which is what a URL-keyed cache needs,
	/// and what a dictionary's iteration order does not give you.
	@Test func parameterOrderIsDeterministic() {
		let one = client(salt: "00").url("getAlbum", parameters: ["id": "7", "size": "64"])
		let two = client(salt: "00").url("getAlbum", parameters: ["size": "64", "id": "7"])
		#expect(one == two)
	}
}
