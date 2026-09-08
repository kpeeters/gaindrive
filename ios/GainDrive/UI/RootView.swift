//	GainDrive for iOS
//	Copyright (C) 2026 Kasper Peeters
//
//	This file is part of GainDrive, distributed under the GNU General
//	Public License version 3 or later, with the Application Distribution
//	Exception in ios/LICENSE. See LICENSE at the repository root for the
//	full text of the GPL.

import SwiftUI

/// The persistent shell: five destinations, matching the web client's bottom
/// bar and `android/SCREENS.md`.
struct RootView: View {
	/// Decided **once**, by the composition root, from the registry as it was
	/// at launch — and then frozen. Re-deriving it from `servers.isEmpty`
	/// would move the user out from under themselves the moment they saved
	/// their first server, resetting the navigation stack mid-task.
	let firstRun: Bool

	@Environment(PlayerConnection.self) private var player
	@Environment(PinRepository.self) private var pins
	@State private var tab: Destination
	/// Raised by any tab's mini player, and owned here so there is exactly one
	/// of it — see `miniPlayerInset`.
	@State private var showingPlayer = false
	/// The tab-root view model is built here rather than inside `ArtistsView`
	/// because a `@State` initial value cannot read `@Environment`, and the
	/// usual workaround — an optional filled in from `.task` — puts a spinner
	/// in front of the tab for a frame and an unwrap at every use site.
	///
	/// This initialiser runs again on **every** re-evaluation of
	/// `GainDriveApp.body`, which `preferredColorScheme` guarantees on each
	/// theme change; `@State` keeps the first value and discards the rest. That
	/// is why a view model's own initialiser must start no work.
	@State private var artists: ArtistsViewModel
	@State private var playlists: PlaylistsViewModel
	@State private var recents: RecentsViewModel
	@State private var search: SearchViewModel

	init(
		firstRun: Bool, library: LibraryRepository, selection: ServerSelection,
		events: LibraryEvents, settings: SettingsStore
	) {
		self.firstRun = firstRun
		_tab = State(initialValue: firstRun ? .settings : .artists)
		_artists = State(
			initialValue: ArtistsViewModel(
				library: library, selection: selection, settings: settings))
		_playlists = State(
			initialValue: PlaylistsViewModel(
				library: library, selection: selection, events: events))
		_recents = State(
			initialValue: RecentsViewModel(library: library, selection: selection))
		_search = State(
			initialValue: SearchViewModel(library: library, selection: selection))
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
				ArtistsView(model: artists)
					.miniPlayerInset { showingPlayer = true }
			}
			Tab("Playlists", systemImage: "music.note.list", value: Destination.playlists) {
				PlaylistsView(model: playlists)
					.miniPlayerInset { showingPlayer = true }
			}
			Tab("Recents", systemImage: "clock.arrow.circlepath", value: Destination.recents) {
				RecentsView(model: recents)
					.miniPlayerInset { showingPlayer = true }
			}
			Tab("Search", systemImage: "magnifyingglass", value: Destination.search) {
				SearchView(model: search)
					.miniPlayerInset { showingPlayer = true }
			}
			Tab("Settings", systemImage: "gearshape", value: Destination.settings) {
				SettingsView(startOnServers: firstRun)
					.miniPlayerInset { showingPlayer = true }
			}
		}
		// The iOS 18 idiom for a tabbed app on a wide screen: the tab bar
		// becomes a sidebar on iPad and under Catalyst, and stays a tab bar on
		// a phone. It is also what makes the Mac build look like a Mac app
		// rather than a stretched phone, which `PLAN.md` lists as the whole
		// point of taking the Catalyst destination.
		.tabViewStyle(.sidebarAdaptable)
		.sheet(isPresented: $showingPlayer) { NowPlayingView() }
		// **Playback errors belong to the shell, not to a screen.** They arrive
		// from the audio session, from an item that failed to load and from the
		// watchdog, none of which is any one screen's business — and a track
		// that could not be played leaves no mini player to hang an alert on,
		// which is exactly when there is something to say.
		.alert(
			"Playback problem",
			isPresented: Binding(
				get: { player.errorMessage != nil },
				set: { if !$0 { player.clearError() } })
		) {
			Button("OK") { player.clearError() }
		} message: {
			Text(player.errorMessage ?? "")
		}
		// Pinning is reached from four listings, not only from the album
		// screen, so a refusal belongs to the shell for the same reason a
		// playback error does.
		.alert(
			"Cannot download that",
			isPresented: Binding(
				get: { pins.message != nil }, set: { if !$0 { pins.clearMessage() } })
		) {
			Button("OK") { pins.clearMessage() }
		} message: {
			Text(pins.message ?? "")
		}
	}
}
