//	GainDrive for macOS
//	Copyright (C) 2026 Kasper Peeters
//
//	This file is part of GainDrive, distributed under the GNU General
//	Public License version 3 or later, with the Application Distribution
//	Exception in ios/LICENSE. See LICENSE at the repository root for the
//	full text of the GPL.

#if os(macOS)

import SwiftUI

/// The Mac's window: a sections column, then the chosen section's panes side
/// by side, the player bar along the bottom and Now Playing as an inspector on
/// the right. The web client's layout, which is why it is not Apple Music's:
/// Music keeps one list beside a detail stack and so hides the level between,
/// and the point here is to see all of them at once.
///
/// `NavigationSplitView` in its two-column form, because it stops at three
/// columns and the panes need more; they are `MacPaneRow`, inside the detail.
/// The window's own toolbar is left empty: every control belongs to a pane's
/// header, as on the other platforms, where no bar spans all the panes.
struct MacRootView: View {
	let firstRun: Bool

	/// Optional only because `List(selection:)` wants it so; nil reads as
	/// Library.
	@State private var section: SidebarItem?
	@State private var showsInspector = false
	/// Built here for the reason `RootView` gives: a `@State` initial value
	/// cannot read `@Environment`.
	@State private var artists: ArtistsViewModel
	@State private var playlists: PlaylistsViewModel
	@State private var recents: RecentsViewModel
	@State private var search: SearchViewModel

	init(
		firstRun: Bool, library: LibraryRepository, selection: ServerSelection,
		events: LibraryEvents
	) {
		self.firstRun = firstRun
		// Nothing to browse without a server, and the servers are in Settings.
		_section = State(initialValue: firstRun ? .settings : .library)
		_artists = State(
			initialValue: ArtistsViewModel(library: library, selection: selection))
		_playlists = State(
			initialValue: PlaylistsViewModel(
				library: library, selection: selection, events: events))
		_recents = State(
			initialValue: RecentsViewModel(library: library, selection: selection))
		_search = State(
			initialValue: SearchViewModel(library: library, selection: selection))
	}

	enum SidebarItem: CaseIterable, Identifiable {
		case library, playlists, recents, search, settings

		var id: Self { self }

		var title: String {
			switch self {
			case .library: "Library"
			case .playlists: "Playlists"
			case .recents: "Recents"
			case .search: "Search"
			case .settings: "Settings"
			}
		}

		var symbol: String {
			switch self {
			case .library: "music.mic"
			case .playlists: "music.note.list"
			case .recents: "clock.arrow.circlepath"
			case .search: "magnifyingglass"
			case .settings: "gearshape"
			}
		}
	}

	var body: some View {
		NavigationSplitView {
			List(SidebarItem.allCases, selection: $section) { section in
				Label(section.title, systemImage: section.symbol)
			}
			.navigationSplitViewColumnWidth(min: 140, ideal: 170, max: 240)
		} detail: {
			detail
				// A title here would span all the panes; each names itself.
				.toolbar(removing: .title)
		}
		// Inside the bottom inset, so the inspector ends above the player bar
		// and the bar spans the whole window, as a transport should.
		.inspector(isPresented: $showsInspector) {
			NowPlayingView()
				.inspectorColumnWidth(min: 280, ideal: 320, max: 440)
		}
		.safeAreaInset(edge: .bottom, spacing: 0) {
			VStack(spacing: 0) {
				FetchStrip()
				MacPlayerBar(showsInspector: $showsInspector)
			}
		}
		.frame(minWidth: 720, minHeight: 480)
		// One window, so it is always the one on screen.
		.modifier(VideoPresentation(active: true))
		// A link opens in Search, which owns a path to push onto, as on iOS.
		.modifier(
			ShellMessages { route in
				section = .search
				search.open(route)
			})
	}

	/// Switched rather than kept alive side by side: a hidden section would
	/// still contribute its toolbar items and search field to the window.
	@ViewBuilder
	private var detail: some View {
		switch section ?? .library {
		case .library: ArtistsView(model: artists)
		case .playlists: PlaylistsView(model: playlists)
		case .recents: RecentsView(model: recents)
		case .search: SearchView(model: search)
		case .settings: SettingsView(startOnServers: firstRun)
		}
	}
}

#endif
