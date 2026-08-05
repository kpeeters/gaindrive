//	GainDrive for iOS
//	Copyright (C) 2026 Kasper Peeters
//
//	This file is part of GainDrive, distributed under the GNU General
//	Public License version 3 or later, with the Application Distribution
//	Exception in ios/LICENSE. See LICENSE at the repository root for the
//	full text of the GPL.

import Foundation

/// One configured server. **The password is not here** — it lives in the
/// Keychain under the server's id, so this record can be written to
/// `UserDefaults`, logged and inspected without leaking anything.
struct ServerConfig: Identifiable, Codable, Hashable, Sendable {
	let id: ServerId
	/// May be empty, in which case `displayName` falls back to the host. Not
	/// filled in eagerly with the host at save time: a user who later moves
	/// the server to a new address should see the new host, not a stale one
	/// frozen into the name field.
	var name: String
	var urlString: String
	var username: String
	var isEnabled: Bool

	init(
		id: ServerId = ServerId(),
		name: String = "",
		urlString: String,
		username: String,
		isEnabled: Bool = true
	) {
		self.id = id
		self.name = ServerConfig.normalisedName(name)
		self.urlString = ServerConfig.normalisedURL(urlString)
		self.username = ServerConfig.normalisedUsername(username)
		self.isEnabled = isEnabled
	}

	var baseURL: URL? { URL(string: urlString) }
	var host: String? { baseURL?.host() }

	var displayName: String {
		if !name.isEmpty { return name }
		if let host, !host.isEmpty { return host }
		return urlString
	}

	/// Host and port, for the second line of a list row — enough to tell two
	/// servers apart when both are named after the same thing.
	var displayAddress: String {
		guard let baseURL, let host = baseURL.host() else { return urlString }
		if let port = baseURL.port { return "\(host):\(port)" }
		return host
	}

	// MARK: - Normalisation

	static func normalisedName(_ raw: String) -> String {
		raw.trimmingCharacters(in: .whitespacesAndNewlines)
	}

	/// See `AuthParameters` for why this is trimmed rather than taken as
	/// typed. It is done here as well so the *stored* value is clean, and a
	/// user comparing two rows is not looking at an invisible difference.
	static func normalisedUsername(_ raw: String) -> String {
		raw.trimmingCharacters(in: .whitespacesAndNewlines)
	}

	/// Trims, supplies a scheme, and strips trailing slashes.
	///
	/// The scheme is supplied because the overwhelmingly common input is a
	/// bare `192.0.2.9:4040` typed from memory, and `URL(string:)` accepts
	/// that as a *relative* URL with no host rather than rejecting it — which
	/// would surface much later as an unexplained request failure. `http` and
	/// not `https`, because a LAN server without TLS is the case that needs
	/// the help; anyone who has TLS types the scheme.
	///
	/// Trailing slashes go because every request appends `rest/<endpoint>`,
	/// and the web client normalises the same way.
	static func normalisedURL(_ raw: String) -> String {
		var text = raw.trimmingCharacters(in: .whitespacesAndNewlines)
		guard !text.isEmpty else { return "" }
		if !text.contains("://") {
			text = "http://" + text
		}
		while text.hasSuffix("/") {
			text.removeLast()
		}
		return text
	}

	/// What the editor's save button is gated on. A URL that does not parse
	/// into a scheme and a host would produce request URLs that fail with
	/// nothing useful to say.
	static func isUsable(urlString: String) -> Bool {
		guard let url = URL(string: urlString), let scheme = url.scheme?.lowercased() else {
			return false
		}
		guard scheme == "http" || scheme == "https" else { return false }
		guard let host = url.host(), !host.isEmpty else { return false }
		return true
	}
}
