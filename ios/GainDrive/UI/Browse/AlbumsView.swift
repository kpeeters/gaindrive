//	GainDrive for iOS
//	Copyright (C) 2026 Kasper Peeters
//
//	This file is part of GainDrive, distributed under the GNU General
//	Public License version 3 or later, with the Application Distribution
//	Exception in ios/LICENSE. See LICENSE at the repository root for the
//	full text of the GPL.

import SwiftUI

struct AlbumsView: View {
	let refs: [ItemRef]
	let artistName: String
	/// See `Route.albums`. Defaulted, so a section reached from search keeps
	/// today's placeholder rather than having the answer guessed.
	var fromCategories = false

	@Environment(\.library) private var library
	@Environment(ServerSelection.self) private var selection
	@Environment(SettingsStore.self) private var settings
	@State private var model: AlbumsViewModel?
	@State private var notesDismissed = false

	var body: some View {
		Group {
			if let model {
				LoadStateBox(state: model.state, onRetry: { model.retry() }) { albums in
					list(albums, model: model)
				}
			} else {
				ProgressView()
			}
		}
		.navigationTitle(artistName)
		.navigationBarTitleDisplayMode(.inline)
		.toolbar {
			if let model {
				ToolbarItem(placement: .topBarTrailing) { sortMenu(model) }
			}
		}
		.task {
			// Built here rather than in an initialiser because a `@State`
			// initial value cannot read `@Environment`. Assigned once, so the
			// model survives every re-evaluation of this body.
			if model == nil, let library {
				model = AlbumsViewModel(
					library: library, selection: selection, settings: settings,
					refs: refs, fromCategories: fromCategories)
			}
			model?.appear()
		}
	}

	/// The server answers in year order alone (`ORDER BY al.year, al.title`),
	/// which is right for a discography and useless for a film category, where
	/// the only thing anyone knows about an item is its name.
	private func sortMenu(_ model: AlbumsViewModel) -> some View {
		Menu {
			ForEach(AlbumSort.allCases, id: \.self) { option in
				Button {
					model.setSort(option)
				} label: {
					Label(
						option.label,
						systemImage: model.sort == option ? "checkmark" : "")
				}
			}
		} label: {
			Label("Sort", systemImage: "arrow.up.arrow.down")
		}
	}

	@ViewBuilder
	private func list(_ albums: [AlbumUi], model: AlbumsViewModel) -> some View {
		if albums.isEmpty {
			EmptyMessage(text: "No albums")
		} else {
			List {
				if model.info != nil || model.portrait != nil {
					header(model)
						.listRowSeparator(.hidden)
				}
				if !model.failures.isEmpty, !notesDismissed {
					PartialFailureNote(
						failures: model.failures,
						onRetry: { Task { await model.refresh() } },
						onDismiss: { notesDismissed = true }
					)
					.listRowSeparator(.hidden)
				}
				ForEach(albums) { item in
					NavigationLink(value: Route.album(item.album.ref, title: item.album.title)) {
						AlbumRow(item: item)
					}
				}
			}
			.listStyle(.plain)
			.refreshable { await model.refresh() }
		}
	}

	private func header(_ model: AlbumsViewModel) -> some View {
		VStack(alignment: .leading, spacing: 12) {
			HStack(spacing: 12) {
				ArtistAvatar(source: model.portrait, size: 72)
				Text(artistName)
					.font(.title3.weight(.semibold))
				Spacer(minLength: 0)
			}
			if let info = model.info {
				NotesSection(text: info.biography, links: info.externalLinks)
			}
		}
		.padding(.vertical, 4)
	}
}
