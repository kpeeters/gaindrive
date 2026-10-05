//	GainDrive for iOS
//	Copyright (C) 2026 Kasper Peeters
//
//	This file is part of GainDrive, distributed under the GNU General
//	Public License version 3 or later, with the Application Distribution
//	Exception in ios/LICENSE. See LICENSE at the repository root for the
//	full text of the GPL.

import Foundation

/// What the signed-in account may do on **one** server.
///
/// Roles are per server - the same person can be an admin on one and a
/// restricted account on another - so nothing here is a global fact about the
/// user.
struct AccountFacts: Hashable, Sendable {
	/// May write into the personal uploads area. Admins may regardless.
	var canUpload = false
	var isAdmin = false

	/// What an unreachable server is assumed to be: permitted nothing. A
	/// permission guessed *present* would offer a control that fails when
	/// used.
	static let unknown = AccountFacts()
}

/// One `getUser` per server per session, answering everything the app needs to
/// know about its own account there.
///
/// `canUpload` decides whether the Uploads chip is offered at all, and
/// `isAdmin` whether it covers everybody's uploads or only this account's.
///
/// Held in memory rather than persisted: it is one cheap call per server per
/// launch, it picks up a change made on the server with no invalidation logic,
/// and there is nothing to migrate.
actor Accounts {
	/// Answers, and separately the requests in flight.
	///
	/// **A failure is not remembered.** Android learned this: resolving a
	/// timeout to `unknown` and storing it would hide the Uploads chip for the
	/// rest of the session, and the only way back would be relaunching.
	/// Re-asking costs one request.
	private var known: [ServerId: AccountFacts] = [:]
	/// The in-flight *task*, not the answer. An actor releases isolation at
	/// every `await`, so two tracks starting together would otherwise both find
	/// the cache empty and both ask.
	private var inFlight: [ServerId: Task<AccountFacts?, Never>] = [:]

	private static let timeout = Duration.seconds(2)

	func facts(for server: ServerId, using clients: ServerClients) async -> AccountFacts {
		if let answer = known[server] { return answer }
		if let existing = inFlight[server] { return await existing.value ?? .unknown }
		guard let client = clients.client(for: server) else { return .unknown }

		let task = Task<AccountFacts?, Never> { await Self.fetch(client) }
		inFlight[server] = task
		let answer = await task.value
		// Re-entered here: another caller may have cleared these already, which
		// is why both writes are idempotent rather than conditional.
		inFlight[server] = nil
		if let answer { known[server] = answer }
		return answer ?? .unknown
	}

	/// Drops a cached answer, so an account whose roles just changed is asked
	/// again.
	func forget(_ server: ServerId) {
		known[server] = nil
		inFlight[server] = nil
	}

	/// Nil when the server did not answer, which is not a fact about it.
	private static func fetch(_ client: SubsonicClient) async -> AccountFacts? {
		await withTaskGroup(of: AccountFacts?.self) { group in
			group.addTask {
				guard let user = try? await client.user() else { return nil }
				return AccountFacts(
					// An admin may upload whether or not the role is set, which
					// is how the server itself reads it - see
					// `check_upload_perm`.
					canUpload: user.canUpload || user.isAdmin,
					isAdmin: user.isAdmin)
			}
			group.addTask {
				try? await Task.sleep(for: Self.timeout)
				// The same answer a failure gives: a server that did not answer
				// in time has told us nothing, and must be asked again.
				return nil
			}
			let first = await group.next() ?? nil
			group.cancelAll()
			return first
		}
	}
}
