//	GainDrive for iOS
//	Copyright (C) 2026 Kasper Peeters
//
//	This file is part of GainDrive, distributed under the GNU General
//	Public License version 3 or later, with the Application Distribution
//	Exception in ios/LICENSE. See LICENSE at the repository root for the
//	full text of the GPL.

import Foundation

/// Which servers the browse screens are showing, mirroring
/// `data/ServerSelection.kt`.
///
/// **Every property here is computed.** There is no stored copy of the scope,
/// the available servers or the badge names, so there is nothing that can fall
/// out of step with the registry - disabling a server in Settings changes what
/// this answers on the next read, with no notification to wire up. Because both
/// `ServerRegistry` and `SettingsStore` are `@Observable`, that propagates to
/// the views transitively.
@MainActor
@Observable
final class ServerSelection {
	@ObservationIgnored private let registry: ServerRegistry
	@ObservationIgnored private let settings: SettingsStore

	init(registry: ServerRegistry, settings: SettingsStore) {
		self.registry = registry
		self.settings = settings
	}

	var available: [ServerConfig] { registry.enabled }

	/// A stored choice whose server was removed or disabled falls back to all
	/// servers rather than leaving the library permanently empty with no hint
	/// why.
	var scope: BrowseScope {
		BrowseScope.restored(from: settings.selectedServer, available: available.map(\.id))
	}

	/// The servers the current scope actually covers, in registry order.
	var scoped: [ServerConfig] {
		switch scope {
		case .allServers: available
		case .oneServer(let id): available.filter { $0.id == id }
		}
	}

	/// Server names by id, for the row badges - **empty unless several servers
	/// are genuinely in play**. Badges therefore suppress themselves both in
	/// single-server scope and when only one server is configured, without any
	/// screen having to know that rule.
	var badgeNames: [ServerId: String] {
		guard scoped.count > 1 else { return [:] }
		return Dictionary(uniqueKeysWithValues: scoped.map { ($0.id, $0.displayName) })
	}

	/// Hidden below two servers: the common case should not pay for the
	/// general one, and a picker with a single entry is a control that does
	/// nothing.
	var showsSelector: Bool { available.count > 1 }

	/// Nothing to browse at all - which a fan-out cannot tell apart from a
	/// library that is simply empty, since both come back with no rows and no
	/// failures.
	var hasNoServers: Bool { available.isEmpty }

	var currentName: String {
		switch scope {
		case .allServers: "All servers"
		case .oneServer(let id): registry.config(for: id)?.displayName ?? "All servers"
		}
	}

	func select(_ id: ServerId) {
		settings.selectedServer = BrowseScope.oneServer(id).stored
	}

	func selectAllServers() {
		settings.selectedServer = BrowseScope.allStored
	}
}
