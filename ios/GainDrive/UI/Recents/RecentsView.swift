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
	@State private var path: [Route] = []
	@State private var notesDismissed = false

	var body: some View {
		NavigationStack(path: $path) {
			LoadStateBox(state: model.state, onRetry: { model.retry() }) { sections in
				content(sections)
			}
			.navigationTitle("Recents")
			.navigationDestination(for: Route.self) { route in
				if case .album(let ref, let title) = route {
					AlbumDetailView(ref: ref, albumTitle: title)
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
		.onChange(of: selection.scope) { path.removeAll() }
	}

	@ViewBuilder
	private func content(_ sections: [ServerSection<SongUi>]) -> some View {
		if sections.isEmpty {
			EmptyMessage(text: "Nothing played yet", symbol: "clock.arrow.circlepath")
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
							SectionHeading(text: section.server.displayName)
						}
					}
				}
			}
			.listStyle(.plain)
			.refreshable { await model.refresh() }
		}
	}

	@ViewBuilder
	private func row(_ item: SongUi) -> some View {
		if let album = item.song.albumRef {
			NavigationLink(value: Route.album(album, title: item.song.albumTitle)) {
				SongRow(item: item, trailingText: relativeTime(item.song.lastPlayedAt))
			}
			.trackActions(for: item.song)
		} else {
			SongRow(item: item, trailingText: relativeTime(item.song.lastPlayedAt))
				.trackActions(for: item.song)
		}
	}
}
