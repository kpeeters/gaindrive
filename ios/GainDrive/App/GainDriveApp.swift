//	GainDrive for iOS
//	Copyright (C) 2026 Kasper Peeters
//
//	This file is part of GainDrive, distributed under the GNU General
//	Public License version 3 or later, with the Application Distribution
//	Exception in ios/LICENSE. See LICENSE at the repository root for the
//	full text of the GPL.

import SwiftUI

/// The composition root. Objects that are genuinely global are built here and
/// handed down through the environment; anything that depends on *which*
/// server is asked of the registry, which is why there is no container and no
/// DI framework — `android/ARCHITECTURE.md` describes Hilt doing manual
/// composition in all but name, for the same reason.
@main
struct GainDriveApp: App {
	@State private var registry: ServerRegistry
	@State private var settings: SettingsStore
	@State private var selection: ServerSelection
	@State private var library: LibraryRepository
	@State private var events: LibraryEvents
	@State private var stars: StarStore
	@State private var player: PlayerConnection

	/// Read once, here, before any view exists. `RootView` explains why this
	/// cannot be derived inside the view hierarchy.
	private let firstRun: Bool

	init() {
		let registry = ServerRegistry()
		let settings = SettingsStore()
		firstRun = registry.servers.isEmpty
		_registry = State(initialValue: registry)
		_settings = State(initialValue: settings)
		let events = LibraryEvents()
		// Shared rather than private to either: the same `getUser` answers the
		// bitrate ceiling a stream URL needs and the roles the chip row needs,
		// and asking twice would be two requests for one fact.
		let accounts = Accounts()
		let roots = MusicRoots()
		let library = LibraryRepository(
			registry: registry, settings: settings, events: events,
			accounts: accounts, roots: roots)
		_selection = State(initialValue: ServerSelection(registry: registry, settings: settings))
		_events = State(initialValue: events)
		_library = State(initialValue: library)
		_stars = State(initialValue: StarStore(library: library))
		// Built here, never in `RootView.init`, which re-runs on every
		// re-evaluation of this body — `@State` would keep the first player and
		// silently discard the rest, each with its own audio session.
		_player = State(
			initialValue: PlayerConnection(
				registry: registry, settings: settings, library: library,
				accounts: accounts))
		// Editing a server may have pointed it at a different account, whose
		// ceiling and roles are otherwise cached from the old one for the rest
		// of the session.
		registry.onServerInvalidated = { id in
			Task { await accounts.forget(id) }
			Task { await roots.forget(id) }
		}
	}

	var body: some Scene {
		WindowGroup {
			RootView(
				firstRun: firstRun, library: library, selection: selection, events: events,
				settings: settings
			)
			.environment(registry)
			.environment(settings)
			.environment(selection)
			.environment(events)
			.environment(stars)
			.environment(player)
			.environment(\.library, library)
			.preferredColorScheme(settings.themeMode.colorScheme)
		}
	}
}
