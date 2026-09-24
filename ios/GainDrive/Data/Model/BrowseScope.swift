//	GainDrive for iOS
//	Copyright (C) 2026 Kasper Peeters
//
//	This file is part of GainDrive, distributed under the GNU General
//	Public License version 3 or later, with the Application Distribution
//	Exception in ios/LICENSE. See LICENSE at the repository root for the
//	full text of the GPL.

import Foundation

/// Which servers a browse screen is showing.
///
/// `allServers` is the default rather than a named server: with one server
/// configured the two are the same thing and the selector is hidden, so **the
/// app still looks like a single-server client until it isn't one**; with
/// several, seeing all of them is the reason they were added.
enum BrowseScope: Hashable, Sendable {
	case allServers
	case oneServer(ServerId)

	/// The stored form. A sentinel rather than an empty string or a flag,
	/// because it has to share a field with a `ServerId` - and `"all"` is not
	/// a UUID, so the two can never collide.
	static let allStored = "all"

	var stored: String {
		switch self {
		case .allServers: Self.allStored
		case .oneServer(let id): id.value.uuidString
		}
	}

	/// Resolves a stored choice against the servers that currently exist.
	///
	/// A choice whose server was removed or disabled falls back to all rather
	/// than leaving the library permanently empty with no hint why - which is
	/// what a scope pointing at a server that is not there would do.
	static func restored(from stored: String?, available: [ServerId]) -> BrowseScope {
		guard let stored, stored != allStored,
			let uuid = UUID(uuidString: stored),
			available.contains(ServerId(uuid))
		else {
			return .allServers
		}
		return .oneServer(ServerId(uuid))
	}
}
