//	GainDrive for iOS
//	Copyright (C) 2026 Kasper Peeters
//
//	This file is part of GainDrive, distributed under the GNU General
//	Public License version 3 or later, with the Application Distribution
//	Exception in ios/LICENSE. See LICENSE at the repository root for the
//	full text of the GPL.

import Foundation

/// The "Test connection" action in the server editor.
///
/// It runs `ping` **and then** `getUser`, which looks redundant and is not.
/// Bandcamp's Subsonic bridge answers `ping` with `ok` whatever credentials it
/// is handed, so a wrong password there passes the obvious test and then fails
/// on the first endpoint that actually looks the account up. `getUser` is that
/// endpoint, it is cheap, and it is the one whose reply the app wants anyway —
/// roles are per server, and this is where they come from.
///
/// Only a credentials failure from `getUser` is fatal to the test. A server
/// that does not implement it, or refuses it, has still proved it is there and
/// listening, and refusing to save a working server over a missing optional
/// endpoint would be worse than not checking at all.
enum ConnectionTest {
	enum Outcome: Sendable, Equatable {
		case ok(SubsonicUser)
		/// Reachable and authenticated, but the account details are unavailable.
		case okWithoutRoles
		case wrongCredentials
		case failed(String)

		var isSuccess: Bool {
			switch self {
			case .ok, .okWithoutRoles: true
			case .wrongCredentials, .failed: false
			}
		}
	}

	static func run(
		baseURL: URL,
		username: String,
		password: String,
		session: URLSession = HTTP.shared
	) async -> Outcome {
		// A scratch id: this client exists for the length of the test and its
		// identity is never used for anything the id would key.
		let client = SubsonicClient(
			serverId: ServerId(),
			baseURL: baseURL,
			auth: AuthParameters(username: username, password: password),
			session: session
		)

		do {
			try await client.ping()
		} catch let error as SubsonicError {
			if case .wrongCredentials = error { return .wrongCredentials }
			return .failed(describeRequestFailure(error))
		} catch {
			return .failed(describeRequestFailure(error))
		}

		do {
			return .ok(try await client.user())
		} catch let error as SubsonicError {
			if case .wrongCredentials = error { return .wrongCredentials }
			return .okWithoutRoles
		} catch {
			// The transport died between the two calls; `ping` succeeding a
			// moment ago does not make the server reachable now.
			return .failed(describeRequestFailure(error))
		}
	}
}
