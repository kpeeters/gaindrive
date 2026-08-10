//	GainDrive for iOS
//	Copyright (C) 2026 Kasper Peeters
//
//	This file is part of GainDrive, distributed under the GNU General
//	Public License version 3 or later, with the Application Distribution
//	Exception in ios/LICENSE. See LICENSE at the repository root for the
//	full text of the GPL.

import Foundation

/// Each server's `maxBitRate` ceiling, asked once per process.
///
/// Held in memory rather than persisted: it is a property of the account as the
/// server currently has it, and a stale ceiling read from disk would cap every
/// stream for a session against a limit that may have been lifted.
///
/// **Uncapped is the right guess when we cannot ask.** Every failure — an
/// unreachable server, a refusal, the timeout — resolves to 0. Guessing a cap
/// that is not there would degrade every stream on that server for the rest of
/// the session, whereas guessing wrong in this direction costs nothing: the
/// server applies its own ceiling regardless of what the client asked for. The
/// only consequence is a cache key naming a quality the bytes are not, which is
/// a phase 5 concern and is why this is worth getting right now.
actor AccountLimits {
	/// The in-flight *task*, not the answer. An actor releases isolation at
	/// every `await`, so two tracks starting together would otherwise both find
	/// the cache empty and both ask.
	private var inFlight: [ServerId: Task<Int, Never>] = [:]

	private static let timeout = Duration.seconds(2)

	func cap(for server: ServerId, using clients: ServerClients) async -> Int {
		if let existing = inFlight[server] { return await existing.value }
		guard let client = clients.client(for: server) else { return 0 }

		let task = Task<Int, Never> { await Self.fetch(client) }
		inFlight[server] = task
		return await task.value
	}

	/// The first play of a session waits for this, which is why the timeout is
	/// short — and why `PlayerConnection` publishes `loadingRef` before
	/// reaching it.
	private static func fetch(_ client: SubsonicClient) async -> Int {
		await withTaskGroup(of: Int.self) { group in
			group.addTask {
				guard let user = try? await client.user(), let cap = user.maxBitRate, cap > 0
				else { return 0 }
				return cap
			}
			group.addTask {
				try? await Task.sleep(for: Self.timeout)
				// The same answer a failure gives, deliberately: a server that
				// did not answer in time is treated as having no ceiling.
				return 0
			}
			let first = await group.next() ?? 0
			group.cancelAll()
			return first
		}
	}
}
