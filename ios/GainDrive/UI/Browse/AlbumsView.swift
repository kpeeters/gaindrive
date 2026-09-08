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
	/// Non-nil inside the Library tab's split view, where choosing an album
	/// fills the third column. **Nil everywhere else**: from Search, Recents
	/// and the Now Playing sheet this list sits in a `NavigationStack` and a
	/// row is a push.
	///
	/// `List` takes an *optional* selection binding, so the list itself needs
	/// no branch — and the declared type here is what pins its selection type
	/// when the binding is nil.
	var selection: Binding<Album?>?

	@Environment(\.library) private var library
	// `servers`, not `selection`: that name belongs to the album selection
	// above, which is what `List(selection:)` wants it called.
	@Environment(ServerSelection.self) private var servers
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
					library: library, selection: servers, settings: settings,
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
			EmptyMessage(text: settings.offlineMode ? "Nothing downloaded yet" : "No albums")
		} else {
			List(selection: selection) {
				// The header and the note are untagged, and so not selectable.
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
					row(item)
				}
			}
			.listStyle(.plain)
			.refreshable { await model.refresh() }
		}
	}

	/// The rows *are* the branch, because the list is not.
	///
	/// A `NavigationLink` inside a selectable list would push as well as
	/// select, so the split view gets a bare row — tagged with the album
	/// itself rather than left to `AlbumUi.id`, so the column above receives
	/// enough to title the detail column without a lookup.
	@ViewBuilder
	private func row(_ item: AlbumUi) -> some View {
		if selection != nil {
			AlbumRow(item: item)
				.tag(item.album)
		} else {
			NavigationLink(value: Route.album(item.album.ref, title: item.album.title)) {
				AlbumRow(item: item)
			}
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
