//	GainDrive for iOS
//	Copyright (C) 2026 Kasper Peeters
//
//	This file is part of GainDrive, distributed under the GNU General
//	Public License version 3 or later, with the Application Distribution
//	Exception in ios/LICENSE. See LICENSE at the repository root for the
//	full text of the GPL.

import SwiftUI

/// The persistent shell: five destinations, matching the web client's bottom
/// bar and `android/SCREENS.md`. Four of them are placeholders until phase 2
/// fills them; the shell exists now so the start-destination routing is a real
/// thing that can be exercised rather than a promise.
struct RootView: View {
	/// Decided **once**, by the composition root, from the registry as it was
	/// at launch — and then frozen. Re-deriving it from `servers.isEmpty`
	/// would move the user out from under themselves the moment they saved
	/// their first server, resetting the navigation stack mid-task.
	let firstRun: Bool

	@State private var tab: Destination

	init(firstRun: Bool) {
		self.firstRun = firstRun
		_tab = State(initialValue: firstRun ? .settings : .artists)
	}

	/// Not called `Tab`: SwiftUI's own `Tab` is what the builder below
	/// constructs, and a nested type of that name shadows it inside this
	/// scope — which surfaces as "cannot be constructed because it has no
	/// accessible initializers" pointing at the wrong thing entirely.
	enum Destination: Hashable {
		case artists, playlists, recents, search, settings
	}

	var body: some View {
		TabView(selection: $tab) {
			Tab("Artists", systemImage: "music.mic", value: Destination.artists) {
				PlaceholderView(title: "Artists", symbol: "music.mic")
			}
			Tab("Playlists", systemImage: "music.note.list", value: Destination.playlists) {
				PlaceholderView(title: "Playlists", symbol: "music.note.list")
			}
			Tab("Recents", systemImage: "clock.arrow.circlepath", value: Destination.recents) {
				PlaceholderView(title: "Recents", symbol: "clock.arrow.circlepath")
			}
			Tab("Search", systemImage: "magnifyingglass", value: Destination.search) {
				PlaceholderView(title: "Search", symbol: "magnifyingglass")
			}
			Tab("Settings", systemImage: "gearshape", value: Destination.settings) {
				SettingsView(startOnServers: firstRun)
			}
		}
	}
}

/// Stands in for a phase 2 screen. It says which one and that it is not built
/// yet, because a blank tab reads as a bug.
struct PlaceholderView: View {
	let title: String
	let symbol: String

	var body: some View {
		NavigationStack {
			ContentUnavailableView {
				Label(title, systemImage: symbol)
			} description: {
				Text("Browsing arrives in phase 2.")
			}
			.navigationTitle(title)
		}
	}
}

#Preview("Configured") {
	RootView(firstRun: false)
		.environment(ServerRegistry())
		.environment(SettingsStore())
}

#Preview("First run") {
	RootView(firstRun: true)
		.environment(ServerRegistry())
		.environment(SettingsStore())
}
