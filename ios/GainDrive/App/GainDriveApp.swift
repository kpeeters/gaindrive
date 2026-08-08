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

	/// Read once, here, before any view exists. `RootView` explains why this
	/// cannot be derived inside the view hierarchy.
	private let firstRun: Bool

	init() {
		let registry = ServerRegistry()
		let settings = SettingsStore()
		firstRun = registry.servers.isEmpty
		_registry = State(initialValue: registry)
		_settings = State(initialValue: settings)
		_selection = State(initialValue: ServerSelection(registry: registry, settings: settings))
		_library = State(initialValue: LibraryRepository(registry: registry, settings: settings))
	}

	var body: some Scene {
		WindowGroup {
			RootView(firstRun: firstRun, library: library, selection: selection)
				.environment(registry)
				.environment(settings)
				.environment(selection)
				.environment(\.library, library)
				.preferredColorScheme(settings.themeMode.colorScheme)
		}
	}
}
