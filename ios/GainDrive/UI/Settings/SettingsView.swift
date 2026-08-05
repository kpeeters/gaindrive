//	GainDrive for iOS
//	Copyright (C) 2026 Kasper Peeters
//
//	This file is part of GainDrive, distributed under the GNU General
//	Public License version 3 or later, with the Application Distribution
//	Exception in ios/LICENSE. See LICENSE at the repository root for the
//	full text of the GPL.

import SwiftUI

/// Categories with a summary line of their current state, each opening a
/// screen of its own — the shape `android/SCREENS.md` settled on, for the same
/// reason: the sections outgrow one screen well before casting, editing and
/// administration arrive.
///
/// There is no "Log out". Removing a server is the equivalent, and with
/// several servers a single logout action has no clear meaning.
struct SettingsView: View {
	/// On a fresh install the user is dropped here with Servers already
	/// pushed, since there is exactly one useful thing to do and that is where
	/// its button is. Consumed once, at construction — see `RootView`.
	let startOnServers: Bool

	@Environment(ServerRegistry.self) private var registry
	@Environment(SettingsStore.self) private var settings
	@State private var path: [Route]

	init(startOnServers: Bool = false) {
		self.startOnServers = startOnServers
		_path = State(initialValue: startOnServers ? [.servers] : [])
	}

	enum Route: Hashable {
		case servers, library, appearance
	}

	var body: some View {
		NavigationStack(path: $path) {
			List {
				Section {
					NavigationLink(value: Route.servers) {
						LabeledContent("Servers", value: serversSummary)
					}
					NavigationLink(value: Route.library) {
						LabeledContent("Library", value: librarySummary)
					}
					NavigationLink(value: Route.appearance) {
						LabeledContent("Appearance", value: settings.themeMode.label)
					}
				}

				// Inline rather than a category of its own: a screen holding
				// one sentence is a tap for nothing.
				Section("About") {
					HStack(spacing: 12) {
						Image("Logo")
							.resizable()
							.scaledToFit()
							.frame(width: 40, height: 40)
							.clipShape(RoundedRectangle(cornerRadius: 9, style: .continuous))
						VStack(alignment: .leading) {
							Text("GainDrive")
							Text(Self.versionText)
								.font(.footnote)
								.foregroundStyle(.secondary)
						}
					}
					Text("A client for your own GainDrive music server.")
						.font(.footnote)
						.foregroundStyle(.secondary)
				}
			}
			.navigationTitle("Settings")
			.navigationDestination(for: Route.self) { route in
				switch route {
				case .servers: ServersSettingsView(startAdding: startOnServers)
				case .library: LibrarySettingsView()
				case .appearance: AppearanceSettingsView()
				}
			}
		}
	}

	private var serversSummary: String {
		let total = registry.servers.count
		guard total > 0 else { return "None" }
		let disabled = total - registry.enabled.count
		let configured = "\(total) configured"
		return disabled > 0 ? "\(configured), \(disabled) disabled" : configured
	}

	private var librarySummary: String {
		settings.mergeDuplicateAlbums ? "Merging duplicates" : "Showing duplicates"
	}

	private static var versionText: String {
		let info = Bundle.main.infoDictionary
		let version = info?["CFBundleShortVersionString"] as? String ?? "?"
		let build = info?["CFBundleVersion"] as? String ?? "?"
		return "Version \(version) (\(build))"
	}
}

#Preview {
	SettingsView()
		.environment(ServerRegistry())
		.environment(SettingsStore())
}
