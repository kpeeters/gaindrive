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
	/// Only so a background download finishing while the app is not running
	/// can be acknowledged — see `AppDelegate`.
	@UIApplicationDelegateAdaptor(AppDelegate.self) private var appDelegate

	@State private var registry: ServerRegistry
	@State private var settings: SettingsStore
	@State private var selection: ServerSelection
	@State private var library: LibraryRepository
	@State private var events: LibraryEvents
	@State private var stars: StarStore
	@State private var pins: PinRepository
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
		// What each browse query last answered, so a server that is not there
		// can still be read. See `LibraryMirror`.
		let mirror = LibraryMirror()
		let library = LibraryRepository(
			registry: registry, settings: settings, events: events,
			accounts: accounts, roots: roots, mirror: mirror)
		_selection = State(initialValue: ServerSelection(registry: registry, settings: settings))
		_events = State(initialValue: events)
		_library = State(initialValue: library)
		_stars = State(initialValue: StarStore(library: library))

		// Downloads. `StreamTargets` is shared with the player deliberately:
		// a download that asked for different bytes than a play would store
		// them under a key nothing looks for.
		let targets = StreamTargets(registry: registry, settings: settings, accounts: accounts)
		let store = AudioStore()
		let queue = DownloadQueue(store: store)
		let pins = PinRepository(
			library: library, store: store, queue: queue, targets: targets, settings: settings)
		_pins = State(initialValue: pins)
		AppDelegate.adopt(queue)
		// The store is the authority on what is held; this is how a track that
		// arrived or was evicted reaches the marks on screen.
		Task { await store.setChangeHandler { Task { @MainActor in pins.storeChanged() } } }
		// Built here, never in `RootView.init`, which re-runs on every
		// re-evaluation of this body — `@State` would keep the first player and
		// silently discard the rest, each with its own audio session.
		_player = State(
			initialValue: PlayerConnection(
				registry: registry, library: library, targets: targets, store: store))
		// Editing a server may have pointed it at a different account, whose
		// ceiling and roles are otherwise cached from the old one for the rest
		// of the session.
		registry.onServerInvalidated = { id in
			Task { await accounts.forget(id) }
			Task { await roots.forget(id) }
			// A removed server's rows go with it, or its library would go on
			// being browsable under a server that is no longer configured.
			Task { await mirror.forget(id) }
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
			.environment(pins)
			.environment(player)
			.environment(\.library, library)
			.preferredColorScheme(settings.themeMode.colorScheme)
			// Re-reads what each pin covers and fetches anything missing, which
			// is what makes a pinned playlist cover a track added since it was
			// pinned — and what re-adopts a background download the system
			// carried on with while the app was not running.
			.task { await pins.refresh() }
		}
	}
}
