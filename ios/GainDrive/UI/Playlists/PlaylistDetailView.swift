//	GainDrive for iOS
//	Copyright (C) 2026 Kasper Peeters
//
//	This file is part of GainDrive, distributed under the GNU General
//	Public License version 3 or later, with the Application Distribution
//	Exception in ios/LICENSE. See LICENSE at the repository root for the
//	full text of the GPL.

import SwiftUI

struct PlaylistDetailView: View {
	let ref: ItemRef
	let playlistName: String

	@Environment(\.library) private var library
	@State private var model: PlaylistDetailViewModel?

	var body: some View {
		Group {
			if let model {
				LoadStateBox(state: model.state, onRetry: { model.retry() }) { songs in
					list(songs, model: model)
				}
			} else {
				ProgressView()
			}
		}
		.navigationTitle(playlistName)
		.navigationBarTitleDisplayMode(.inline)
		.task {
			if model == nil, let library {
				model = PlaylistDetailViewModel(library: library, ref: ref)
			}
			model?.appear()
		}
		.alert(
			"Could not remove the track",
			isPresented: Binding(
				get: { model?.error != nil },
				set: { if !$0 { model?.clearError() } })
		) {
			Button("OK") { model?.clearError() }
		} message: {
			Text(model?.error ?? "")
		}
	}

	@ViewBuilder
	private func list(_ songs: [SongUi], model: PlaylistDetailViewModel) -> some View {
		if songs.isEmpty {
			EmptyMessage(text: "This playlist is empty")
		} else {
			List {
				// Indices, because removal is positional — the row has to know
				// where it sits, not just what it is. A track added twice
				// appears twice, so the id alone would not be unique either.
				ForEach(Array(songs.enumerated()), id: \.offset) { index, item in
					SongRow(item: item)
						.trackActions(for: item.song)
						.swipeActions(edge: .trailing) {
							Button(role: .destructive) {
								Task { await model.remove(at: index) }
							} label: {
								Label("Remove", systemImage: "minus.circle")
							}
						}
				}
			}
			.listStyle(.plain)
			// While a removal is in flight the positions on screen no longer
			// match the server's, so a second one must not be startable.
			.disabled(model.isRemoving)
			.refreshable { await model.refresh() }
		}
	}
}
