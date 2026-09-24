//	GainDrive for iOS
//	Copyright (C) 2026 Kasper Peeters
//
//	This file is part of GainDrive, distributed under the GNU General
//	Public License version 3 or later, with the Application Distribution
//	Exception in ios/LICENSE. See LICENSE at the repository root for the
//	full text of the GPL.

import Foundation

/// Everything a query needs from the registry, taken once, at the top.
///
/// The registry is `@MainActor` and the fan-out deliberately is not. Reaching
/// back for a client per branch would mean a main-actor hop per server per
/// request and - worse - **a registry edited mid-query would hand one branch a
/// different world from another's**, so a merged list could be built from two
/// different sets of servers. The snapshot is not an optimisation; it is the
/// consistency guarantee.
///
/// Android's `SubsonicClientFactory` is a suspend lookup per call and needs
/// neither property, which is why this has a different name for the same job.
struct ServerClients: Sendable {
	/// Enabled servers, **in registry order** - which is the merge tie-break,
	/// so the order is data rather than presentation.
	let servers: [ServerConfig]

	private let clients: [ServerId: SubsonicClient]

	init(servers: [ServerConfig], clients: [ServerId: SubsonicClient]) {
		self.servers = servers
		self.clients = clients
	}

	func client(for id: ServerId) -> SubsonicClient? { clients[id] }

	func config(for id: ServerId) -> ServerConfig? { servers.first { $0.id == id } }

	func servers(in scope: BrowseScope) -> [ServerConfig] {
		switch scope {
		case .allServers: servers
		case .oneServer(let id): servers.filter { $0.id == id }
		}
	}

	var coverUrls: CoverUrls { CoverUrls(clients: clients) }
}

extension ServerRegistry {
	/// Built from the existing cached `client(for:)`, so no client is ever
	/// constructed anywhere else and the per-session salt stays stable.
	func clientsSnapshot() -> ServerClients {
		let enabled = enabled
		var clients: [ServerId: SubsonicClient] = [:]
		for config in enabled {
			clients[config.id] = client(for: config.id)
		}
		return ServerClients(servers: enabled, clients: clients)
	}
}
