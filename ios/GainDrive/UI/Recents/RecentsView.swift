//	GainDrive for iOS
//	Copyright (C) 2026 Kasper Peeters
//
//	This file is part of GainDrive, distributed under the GNU General
//	Public License version 3 or later, with the Application Distribution
//	Exception in ios/LICENSE. See LICENSE at the repository root for the
//	full text of the GPL.

import SwiftUI

struct RecentsView: View {
	let model: RecentsViewModel

	@Environment(ServerSelection.self) private var selection
	@Environment(SettingsStore.self) private var settings
	@State private var path: [Route] = []
	/// The album picked in the back-filled artist pane; see
	/// `Route.albumSelection`.
	@State private var chosenAlbum: Album?
	@State private var notesDismissed = false

	var body: some View {
		// Three levels at most: the list, the artist (filled in once the album
		// has loaded), and the album.
		PaneNavigator(
			path: $path, maxLevels: 3,
			root: { list },
			destination: { destination($0) },
			placeholder: { _ in
				ContentUnavailableView("Choose a track", systemImage: "clock.arrow.circlepath")
			}
		)
		.task(id: selection.scope) { model.appear() }
		.onChange(of: settings.offlineMode) { model.retry() }
		.onChange(of: selection.scope) { path.removeAll() }
	}

	@ViewBuilder
	private func content(_ sections: [ServerSection<SongUi>]) -> some View {
		if sections.isEmpty {
			// Recents is the server's record, so offline there is nothing to
			// show rather than nothing to have played.
			EmptyMessage(
				text: settings.offlineMode ? "Not available offline" : "Nothing played yet",
				symbol: "clock.arrow.circlepath")
		} else {
			List(selection: PaneSelection.selection($path, level: 0)) {
				if !model.failures.isEmpty, !notesDismissed {
					PartialFailureNote(
						failures: model.failures,
						onRetry: { Task { await model.refresh() } },
						onDismiss: { notesDismissed = true }
					)
					.listRowSeparator(.hidden)
				}
				ForEach(sections) { section in
					Section {
						// A track played twice appears twice, so the ref alone
						// is not a unique identity here.
						ForEach(Array(section.items.enumerated()), id: \.offset) { _, item in
							row(item)
						}
					} header: {
						if sections.count > 1 {
							SectionHeading(text: section.server.displayName).pinnedHeaderBackground()
						}
					}
				}
			}
			.listStyle(.plain)
			.refreshable { await model.refresh() }
		}
	}

	private var list: some View {
		LoadStateBox(state: model.state, onRetry: { model.retry() }) { sections in
			content(sections)
		}
		.paneHeader("Recents") {
			LibrarySelector()
		} actions: {
			Button {
				Task { await model.refresh() }
			} label: {
				Label("Refresh", systemImage: "arrow.clockwise")
			}
		}
	}

	@ViewBuilder
	private func destination(_ route: Route) -> some View {
		switch route {
		case .album(let ref, let title, let autoPlay, let at):
			AlbumDetailView(
				ref: ref, albumTitle: title, autoPlay: autoPlay, autoPlayAt: at,
				onAlbumLoaded: backfill)
		case .albums(let artists, let name, let fromCategories, let fromUploads):
			// Always the back-filled level, at the front of the path, so the
			// album it opens is the one after it.
			AlbumsView(
				refs: artists, artistName: name,
				fromCategories: fromCategories, fromUploads: fromUploads,
				selection: Route.albumSelection($path, level: 1, chosen: $chosenAlbum))
		case .playlist:
			EmptyView()
		}
	}

	/// Puts the artist in front of an album opened from the list, so the
	/// middle pane shows where it came from - the web client's and Android's
	/// layout. Only for an album opened straight from the list: one reached
	/// through the artist already has it.
	private func backfill(_ album: Album) {
		guard path.count == 1, case .album(let ref, _, _, _) = path[0], ref == album.ref,
			let artist = album.artistRef
		else { return }
		path.insert(.albums(artists: [artist], name: album.artistName), at: 0)
	}

	/// Opens the song's album **and starts it there**, which is what the web
	/// client's `viewTracksFromSearch` does and what Android does. A row with no
	/// album to open stays inert rather than playing in place; see
	/// `Route.album` for why playing a hit where it stands is the wrong tier.
	@ViewBuilder
	private func row(_ item: SongUi) -> some View {
		if let album = item.song.albumRef {
			SongRow(item: item, trailingText: relativeTime(item.song.lastPlayedAt))
				.tag(Route.album(album, title: item.song.albumTitle, autoPlay: item.song.ref))
				.trackActions(for: item.song)
		} else {
			SongRow(item: item, trailingText: relativeTime(item.song.lastPlayedAt))
				.trackActions(for: item.song)
		}
	}
}
