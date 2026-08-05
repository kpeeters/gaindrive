//	GainDrive for iOS
//	Copyright (C) 2026 Kasper Peeters
//
//	This file is part of GainDrive, distributed under the GNU General
//	Public License version 3 or later, with the Application Distribution
//	Exception in ios/LICENSE. See LICENSE at the repository root for the
//	full text of the GPL.

import Foundation

/// A server that did not answer, and why, in words a screen can show.
///
/// It carries a **sentence, not an `Error`**, for two reasons that happen to
/// agree. An existential `any Error` is not `Sendable`, so a `Result` holding
/// one cannot cross a task-group boundary at all; and the sentence is what
/// every consumer wanted anyway, so converting inside the child task is both
/// the only thing that compiles and the right place to do it.
struct ServerFailure: Identifiable, Hashable, Sendable {
	let server: ServerId
	let serverName: String
	let message: String

	var id: ServerId { server }
}

/// The result of asking several servers at once: what came back, and who did
/// not answer.
///
/// Partial failure is representable from the start rather than bolted on
/// later. **A screen must never be blank because the least important of three
/// servers is down**, and a type that could only be "the items" or "an error"
/// would make that impossible to express.
struct MergedResult<Value: Sendable>: Sendable {
	let items: Value
	let failures: [ServerFailure]

	init(items: Value, failures: [ServerFailure] = []) {
		self.items = items
		self.failures = failures
	}

	var isPartial: Bool { !failures.isEmpty }

	func map<Mapped: Sendable>(_ transform: (Value) -> Mapped) -> MergedResult<Mapped> {
		MergedResult<Mapped>(items: transform(items), failures: failures)
	}
}

/// One server's contribution, kept in its own section rather than interleaved.
///
/// Playlists, starred items and recents are per-account server-side state. A
/// global ordering across them would look right and be wrong: each server only
/// knows what happened against it, so interleaving recents by timestamp would
/// imply a completeness that does not exist.
struct ServerSection<Item: Sendable>: Identifiable, Sendable {
	let server: ServerConfig
	let items: [Item]

	var id: ServerId { server.id }
}

extension ServerSection: Hashable where Item: Hashable {}

extension MergedResult where Value: Collection {
	/// The decision every browse view model makes, in one place so the four of
	/// them cannot drift:
	///
	/// **Every server failing is a failed screen; some of them failing is a
	/// note over the ones that worked.**
	///
	/// A constrained property rather than a static on `Load`, because a static
	/// member of a generic enum leaves that enum's own parameter unbound at the
	/// call site.
	var load: Load<Value> {
		if items.isEmpty, let first = failures.first {
			return .failed(first.message)
		}
		return .ready(items)
	}
}
