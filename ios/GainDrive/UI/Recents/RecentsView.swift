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
	@State private var notesDismissed = false

	var body: some View {
		NavigationStack(path: $path) {
			LoadStateBox(state: model.state, onRetry: { model.retry() }) { sections in
				content(sections)
			}
			.navigationTitle("Recents")
			.navigationDestination(for: Route.self) { route in
				if case .album(let ref, let title, let autoPlay, let at) = route {
					AlbumDetailView(ref: ref, albumTitle: title, autoPlay: autoPlay, autoPlayAt: at)
				}
			}
			.toolbar {
				ToolbarItem(placement: .topBarLeading) { LibrarySelector() }
				ToolbarItem(placement: .topBarTrailing) {
					Button {
						Task { await model.refresh() }
					} label: {
						Label("Refresh", systemImage: "arrow.clockwise")
					}
				}
			}
		}
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
			List {
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

	/// Opens the song's album **and starts it there**, which is what the web
	/// client's `viewTracksFromSearch` does and what Android does. A row with no
	/// album to open stays inert rather than playing in place; see
	/// `Route.album` for why playing a hit where it stands is the wrong tier.
	@ViewBuilder
	private func row(_ item: SongUi) -> some View {
		if let album = item.song.albumRef {
			NavigationLink(
				value: Route.album(
					album, title: item.song.albumTitle, autoPlay: item.song.ref)
			) {
				SongRow(item: item, trailingText: relativeTime(item.song.lastPlayedAt))
			}
			.trackActions(for: item.song)
		} else {
			SongRow(item: item, trailingText: relativeTime(item.song.lastPlayedAt))
				.trackActions(for: item.song)
		}
	}
}
