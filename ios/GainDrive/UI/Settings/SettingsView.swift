//	GainDrive for iOS
//	Copyright (C) 2026 Kasper Peeters
//
//	This file is part of GainDrive, distributed under the GNU General
//	Public License version 3 or later, with the Application Distribution
//	Exception in ios/LICENSE. See LICENSE at the repository root for the
//	full text of the GPL.

import SwiftUI

/// Categories with a summary line of their current state, each opening a
/// screen of its own - the shape `android/SCREENS.md` settled on, for the same
/// reason: the sections outgrow one screen well before casting, editing and
/// administration arrive.
///
/// There is no "Log out". Removing a server is the equivalent, and with
/// several servers a single logout action has no clear meaning.
struct SettingsView: View {
	/// On a fresh install the user is dropped here with Servers already
	/// pushed, since there is exactly one useful thing to do and that is where
	/// its button is. Consumed once, at construction - see `RootView`.
	let startOnServers: Bool

	@Environment(ServerRegistry.self) private var registry
	@Environment(SettingsStore.self) private var settings
	@Environment(PinRepository.self) private var pins
	@State private var path: [Route]

	init(startOnServers: Bool = false) {
		self.startOnServers = startOnServers
		_path = State(initialValue: startOnServers ? [.servers] : [])
	}

	enum Route: Hashable {
		case servers, library, playback, casting, storage, appearance
	}

	var body: some View {
		// Two levels: the categories, and the one open beside them on a wide
		// screen, as the web client's settings are laid out.
		PaneNavigator(
			path: $path, maxLevels: 2,
			root: { categories },
			destination: { destination($0) }
		)
	}

	private var categories: some View {
		List(selection: PaneSelection.selection($path, level: 0)) {
			Section {
				LabeledContent("Servers", value: serversSummary)
					.paneDisclosure()
					.tag(Route.servers)
				LabeledContent("Library", value: librarySummary)
					.paneDisclosure()
					.tag(Route.library)
				LabeledContent("Playback", value: settings.audioQuality.label)
					.paneDisclosure()
					.tag(Route.playback)
				LabeledContent("Casting", value: castingSummary)
					.paneDisclosure()
					.tag(Route.casting)
				LabeledContent("Storage", value: storageSummary)
					.paneDisclosure()
					.tag(Route.storage)
				LabeledContent("Appearance", value: settings.themeMode.label)
					.paneDisclosure()
					.tag(Route.appearance)
			}

			// Inline rather than a category of its own: a screen holding
			// one sentence is a tap for nothing. The GPL asks that an
			// interactive program say it is free software and where the
			// terms are, and this is the only place the app has to say so.
			// The text is Android's; only the website is a control, so it
			// is the only part in link colour.
			Section("About") {
				HStack(alignment: .top, spacing: 12) {
					Image("Logo")
						.resizable()
						.scaledToFit()
						.frame(width: 40, height: 40)
						.clipShape(RoundedRectangle(cornerRadius: 9, style: .continuous))
					VStack(alignment: .leading, spacing: 4) {
						Text(Self.aboutText)
							.foregroundStyle(.secondary)
						Link(Self.website, destination: URL(string: Self.website)!)
					}
					.font(.footnote)
				}
			}
		}
		.sidebarRows()
		.paneHeader("Settings")
	}

	@ViewBuilder
	private func destination(_ route: Route) -> some View {
		switch route {
		case .servers: ServersSettingsView(startAdding: startOnServers)
		case .library: LibrarySettingsView()
		case .playback: PlaybackSettingsView()
		case .casting: CastSettingsView()
		case .storage: StorageSettingsView()
		case .appearance: AppearanceSettingsView()
		}
	}

	private var serversSummary: String {
		let total = registry.servers.count
		guard total > 0 else { return "None" }
		let disabled = total - registry.enabled.count
		let configured = "\(total) configured"
		return disabled > 0 ? "\(configured), \(disabled) disabled" : configured
	}

	/// The one setting the pane holds. Devices are not counted: browsing runs
	/// only while the picker is open, so any figure would be stale.
	private var castingSummary: String {
		settings.castOriginal ? "Original quality" : "Same as playback"
	}

	/// The figure, not the word "Storage" said twice - a summary line states
	/// the current value, which is the whole reason these rows have one.
	private var storageSummary: String {
		PinRepository.readable(pins.usageBytes)
	}

	private var librarySummary: String {
		settings.mergeDuplicateAlbums ? "Merging duplicates" : "Showing duplicates"
	}

	private static let website = "https://www.gaindrive.org"

	private static var aboutText: String {
		let version = Bundle.main.infoDictionary?["CFBundleShortVersionString"]
			as? String ?? "?"
		return """
			GainDrive \(version)
			A self-hosted, OpenSubsonic-compatible music client.
			Copyright (C) 2026  Kasper Peeters
			Licensed under the GNU General Public License, version 3 or later.
			"""
	}
}
