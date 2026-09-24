//	GainDrive for iOS
//	Copyright (C) 2026 Kasper Peeters
//
//	This file is part of GainDrive, distributed under the GNU General
//	Public License version 3 or later, with the Application Distribution
//	Exception in ios/LICENSE. See LICENSE at the repository root for the
//	full text of the GPL.

import Foundation

/// Each server's configured roots, fetched once and kept for the session.
///
/// Roots are **configuration**: they change when someone edits the server, not
/// while someone is browsing it. Re-asking on every chip press would put a
/// request in front of a control that should feel instant.
///
/// A store rather than a fetcher, unlike `Accounts`: the caller does the
/// request and only hands back what succeeded. That is what keeps the rule
/// simple - **a failure caches nothing**, so a server that was briefly
/// unreachable is asked again rather than remembered as having no roots at all,
/// which would quietly hold its chips back for the rest of the session.
///
/// It carries no in-flight deduplication, and `Accounts` does. The difference
/// is what waits on them: the first stream URL of a session blocks on an
/// account's ceiling, where two concurrent asks would be two requests before
/// the first note. Roots are read by a fan-out that already asks each server
/// once, so the worst case is one extra `getMusicFolders` - cheap, idempotent,
/// and not worth the machinery.
actor MusicRoots {
	private var cache: [ServerId: [MusicRoot]] = [:]

	func cached(_ server: ServerId) -> [MusicRoot]? {
		cache[server]
	}

	func store(_ roots: [MusicRoot], for server: ServerId) {
		cache[server] = roots
	}

	func forget(_ server: ServerId) {
		cache[server] = nil
	}
}
