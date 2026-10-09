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
/// by side, and the player bar along the bottom. The web client's layout, which is why it is not Apple Music's:
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
	@State private var columns: NavigationSplitViewVisibility = .all
	@Environment(PlayerConnection.self) private var player
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

	enum SidebarItem: CaseIterable, Identifiable, Hashable {
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
		// Stacked rather than a bottom `safeAreaInset`: an inset only shifts
		// the safe area, and the panes' lists went on scrolling underneath the
		// bar. Stacked, everything above ends where the bar begins.
		VStack(spacing: 0) {
			split
			FetchStrip()
			PlayerBar()
		}
		.frame(minWidth: 720, minHeight: 480)
		.environment(\.revealAlbum) { reveal($0, track: $1) }
		// For the menu bar; see `MacCommands`.
		.focusedSceneValue(\.shellSection, $section)
		// Space plays and pauses, as in every Mac player. Here rather than as
		// the menu's key equivalent so a focused text field gets it first.
		.onKeyPress(.space) {
			guard player.current != nil else { return .ignored }
			player.togglePlayPause()
			return .handled
		}
		// One window, so it is always the one on screen.
		.modifier(VideoPresentation(active: true))
		// A link opens in Search, which owns a path to push onto, as on iOS.
		.modifier(ShellMessages { open($0) })
	}

	private func open(_ route: Route) {
		section = .search
		search.open(route)
	}

	private func reveal(_ album: ItemRef, track: ItemRef?) {
		section = .library
		artists.reveal(album: album, track: track)
	}

	/// Above the player bar, so the bar spans the whole window, as a
	/// transport should.
	private var split: some View {
		NavigationSplitView(columnVisibility: $columns) {
			List(SidebarItem.allCases, selection: $section) { section in
				Label(section.title, systemImage: section.symbol)
			}
			// One selector for every section, rather than one in each root
			// pane's header: the scope is the window's, not a pane's.
			.safeAreaInset(edge: .bottom, spacing: 0) {
				LibrarySelector()
					.padding(12)
					.frame(maxWidth: .infinity, alignment: .leading)
			}
			// After the inset, so the library selector sits on it too.
			.scrollContentBackground(.hidden)
			.background(Color.sidebarBackground)
			.navigationSplitViewColumnWidth(min: 140, ideal: 170, max: 240)
		} detail: {
			detail
				// The panes run up into the title bar, so their headers are the
				// top of the window, as Xcode's editors are; a strip spanning
				// them all exists on no other platform.
				.ignoresSafeArea(.container, edges: .top)
				// With the sidebar collapsed, the traffic lights and the
				// sidebar toggle sit over the first pane's header.
				.environment(\.paneHeaderLeadingInset, columns == .detailOnly ? 110 : 0)
				.toolbar(removing: .title)
				.toolbarBackgroundVisibility(.hidden, for: .windowToolbar)
		}
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
