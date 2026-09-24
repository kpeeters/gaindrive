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

struct ServerConfigTests {
	@Test(arguments: [
		("192.0.2.9:4040", "http://192.0.2.9:4040"),
		("  192.0.2.9:4040  ", "http://192.0.2.9:4040"),
		("http://192.0.2.9:4040/", "http://192.0.2.9:4040"),
		("http://192.0.2.9:4040///", "http://192.0.2.9:4040"),
		("https://music.example.com", "https://music.example.com"),
		("https://example.com/music/", "https://example.com/music"),
		("HTTPS://example.com", "HTTPS://example.com"),
		("", ""),
	])
	func normalisesTheURL(input: String, expected: String) {
		#expect(ServerConfig.normalisedURL(input) == expected)
	}

	/// A bare host reaches `URL(string:)` as a *relative* URL with no host
	/// rather than being rejected, so without the supplied scheme this would
	/// fail much later as an unexplained request failure.
	@Test func aBareHostBecomesUsable() {
		#expect(ServerConfig.isUsable(urlString: "192.0.2.9:4040") == false)
		#expect(ServerConfig.isUsable(urlString: ServerConfig.normalisedURL("192.0.2.9:4040")))
	}

	/// Note what is *not* here: a bare word like `nonsense`. It normalises to
	/// `http://nonsense`, which is a perfectly well-formed address for a host
	/// on the local network - whether it resolves is the connection test's
	/// question, not this one's.
	@Test(arguments: ["", "   ", "ftp://example.com", "http://", ":4040"])
	func rejectsUnusableAddresses(input: String) {
		#expect(ServerConfig.isUsable(urlString: ServerConfig.normalisedURL(input)) == false)
	}

	@Test func displayNameFallsBackToTheHost() {
		let unnamed = ServerConfig(urlString: "http://192.0.2.9:4040", username: "admin")
		#expect(unnamed.displayName == "192.0.2.9")

		let named = ServerConfig(name: "  Loft  ", urlString: "http://192.0.2.9:4040", username: "admin")
		#expect(named.displayName == "Loft")
	}

	/// The address line in a list row has to include the port, or two servers
	/// on one machine look identical.
	@Test func displayAddressKeepsThePort() {
		let config = ServerConfig(urlString: "192.0.2.9:4040", username: "admin")
		#expect(config.displayAddress == "192.0.2.9:4040")
	}

	@Test func trimsTheUsername() {
		let config = ServerConfig(urlString: "http://h", username: " admin\n")
		#expect(config.username == "admin")
	}

	/// The password must never be in a record that gets written to
	/// UserDefaults, logged or inspected.
	@Test func encodedFormCarriesNoSecret() throws {
		let config = ServerConfig(name: "Loft", urlString: "http://h:4040", username: "admin")
		let data = try JSONEncoder().encode(config)
		let json = try #require(String(data: data, encoding: .utf8))
		#expect(!json.lowercased().contains("password"))
		#expect(json.contains("admin"))
	}

	@Test func survivesAPersistenceRoundTrip() throws {
		let original = ServerConfig(
			name: "Loft", urlString: "http://192.0.2.9:4040", username: "admin", isEnabled: false)
		let data = try JSONEncoder().encode([original])
		let decoded = try JSONDecoder().decode([ServerConfig].self, from: data)
		#expect(decoded == [original])
		#expect(decoded.first?.isEnabled == false)
	}
}
