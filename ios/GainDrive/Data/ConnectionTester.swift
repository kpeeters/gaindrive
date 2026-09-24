//	GainDrive for iOS
//	Copyright (C) 2026 Kasper Peeters
//
//	This file is part of GainDrive, distributed under the GNU General
//	Public License version 3 or later, with the Application Distribution
//	Exception in ios/LICENSE. See LICENSE at the repository root for the
//	full text of the GPL.

import Foundation

/// Runs the "Test connection" action in the server editor. The counterpart of
/// `android/…/data/ConnectionTester.kt`, and in the same layer for the same
/// reason: it is a question about a *configuration*, asked before any client
/// exists for it.
///
/// It runs `ping` **and then** `getUser`, which looks redundant and is not.
/// Bandcamp's Subsonic bridge answers `ping` with `ok` whatever credentials it
/// is handed, so a wrong password there passes the obvious test and then fails
/// on the first endpoint that actually looks the account up. `getUser` is that
/// endpoint, it is cheap, and it is the one whose reply the app wants anyway -
/// roles are per server, and this is where they come from.
///
/// Only a credentials failure from `getUser` is fatal. A server that does not
/// implement it, or refuses it, has still proved it is there and listening, so
/// that is `unverified` rather than a failure - see `ConnectionTest`.
enum ConnectionTester {
	static func test(
		url: URL,
		username: String,
		password: String,
		session: URLSession = HTTP.shared
	) async -> ConnectionTest {
		// A scratch id: this client exists for the length of the test and its
		// identity is never used for anything the id would key.
		let client = SubsonicClient(
			serverId: ServerId(),
			baseURL: url,
			auth: AuthParameters(username: username, password: password),
			session: session
		)

		do {
			try await client.ping()
		} catch let error as SubsonicError {
			if case .wrongCredentials = error { return .rejected(error.userMessage) }
			return .unreachable(error.userMessage)
		} catch {
			return .unreachable(error.userMessage)
		}

		do {
			return .reachable(try await client.user())
		} catch let error as SubsonicError {
			if case .wrongCredentials = error { return .rejected(error.userMessage) }
			return .unverified
		} catch {
			// The transport died between the two calls; `ping` succeeding a
			// moment ago does not make the server reachable now.
			return .unreachable(error.userMessage)
		}
	}
}
