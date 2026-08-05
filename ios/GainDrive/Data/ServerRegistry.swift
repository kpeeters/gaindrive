//	GainDrive for iOS
//	Copyright (C) 2026 Kasper Peeters
//
//	This file is part of GainDrive, distributed under the GNU General
//	Public License version 3 or later, with the Application Distribution
//	Exception in ios/LICENSE. See LICENSE at the repository root for the
//	full text of the GPL.

import Foundation

/// The single source of truth for which servers exist.
///
/// Repositories take a `ServerId` and ask here for the client; nothing above
/// this layer knows `URLSession` exists. Server-dependent objects cannot be
/// plain singletons, because the set of servers changes at runtime — the
/// registry hands out per-server clients itself, which is the whole of the
/// dependency injection this app needs.
@MainActor
@Observable
final class ServerRegistry {
	private(set) var servers: [ServerConfig] = []

	/// Cached per server, and **the reason the salt is stable**: an
	/// `AuthParameters` generates its salt once, so a client that survives for
	/// the app session keeps every cover-art URL byte-identical across a
	/// scroll. Rebuilding the client per request would silently defeat the
	/// image cache. Dropped when the configuration changes, since the URL,
	/// username or password may have.
	@ObservationIgnored private var clients: [ServerId: SubsonicClient] = [:]
	@ObservationIgnored private let defaults: UserDefaults
	@ObservationIgnored private let session: URLSession

	private static let storageKey = "servers"

	init(defaults: UserDefaults = .standard, session: URLSession = HTTP.shared) {
		self.defaults = defaults
		self.session = session
		servers = Self.load(from: defaults)
	}

	var enabled: [ServerConfig] {
		servers.filter(\.isEnabled)
	}

	func config(for id: ServerId) -> ServerConfig? {
		servers.first { $0.id == id }
	}

	// MARK: - Editing

	@discardableResult
	func add(name: String, urlString: String, username: String, password: String) -> ServerConfig {
		let config = ServerConfig(name: name, urlString: urlString, username: username)
		Keychain.setPassword(password.trimmingCharacters(in: .whitespacesAndNewlines), for: config.id)
		servers.append(config)
		persist()
		return config
	}

	/// `newPassword` is `nil` when the user did not touch the password field,
	/// which must leave the stored one alone — an editor that saved an empty
	/// field as an empty password would lock the user out of a server they
	/// only meant to rename.
	func update(_ config: ServerConfig, newPassword: String?) {
		guard let index = servers.firstIndex(where: { $0.id == config.id }) else { return }
		servers[index] = config
		if let newPassword {
			Keychain.setPassword(newPassword.trimmingCharacters(in: .whitespacesAndNewlines), for: config.id)
		}
		clients[config.id] = nil
		persist()
	}

	func setEnabled(_ isEnabled: Bool, for id: ServerId) {
		guard let index = servers.firstIndex(where: { $0.id == id }) else { return }
		servers[index].isEnabled = isEnabled
		persist()
	}

	func remove(id: ServerId) {
		servers.removeAll { $0.id == id }
		clients[id] = nil
		Keychain.removePassword(for: id)
		persist()
	}

	func remove(atOffsets offsets: IndexSet) {
		for id in offsets.map({ servers[$0].id }) {
			remove(id: id)
		}
	}

	/// The order is not cosmetic: it is the tie-break for merged lists and the
	/// order of per-server sections, so preference between two servers holding
	/// the same album is expressed by moving a row rather than by a separate
	/// favourite-server setting that could disagree with it.
	func move(fromOffsets source: IndexSet, toOffset destination: Int) {
		servers.move(fromOffsets: source, toOffset: destination)
		persist()
	}

	// MARK: - Clients

	func client(for id: ServerId) -> SubsonicClient? {
		if let cached = clients[id] { return cached }
		guard let config = config(for: id),
			let baseURL = config.baseURL,
			let password = Keychain.password(for: id)
		else {
			return nil
		}
		let client = SubsonicClient(
			serverId: id,
			baseURL: baseURL,
			auth: AuthParameters(username: config.username, password: password),
			session: session
		)
		clients[id] = client
		return client
	}

	func password(for id: ServerId) -> String? {
		Keychain.password(for: id)
	}

	// MARK: - Persistence

	private func persist() {
		guard let data = try? JSONEncoder().encode(servers) else { return }
		defaults.set(data, forKey: Self.storageKey)
	}

	/// A malformed or unreadable document yields an empty list rather than a
	/// crash. The user sees "no servers configured" and can add one, which is
	/// recoverable; a launch-time trap is not.
	private static func load(from defaults: UserDefaults) -> [ServerConfig] {
		guard let data = defaults.data(forKey: storageKey),
			let decoded = try? JSONDecoder().decode([ServerConfig].self, from: data)
		else {
			return []
		}
		return decoded
	}
}
