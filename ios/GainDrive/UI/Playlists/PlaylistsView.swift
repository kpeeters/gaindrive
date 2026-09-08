//	GainDrive for iOS
//	Copyright (C) 2026 Kasper Peeters
//
//	This file is part of GainDrive, distributed under the GNU General
//	Public License version 3 or later, with the Application Distribution
//	Exception in ios/LICENSE. See LICENSE at the repository root for the
//	full text of the GPL.

import SwiftUI

struct PlaylistsView: View {
	let model: PlaylistsViewModel

	@Environment(ServerSelection.self) private var selection
	@Environment(SettingsStore.self) private var settings
	@Environment(LibraryEvents.self) private var events
	@State private var path: [Route] = []
	@State private var confirmingDelete: Playlist?
	@State private var notesDismissed = false

	var body: some View {
		NavigationStack(path: $path) {
			LoadStateBox(state: model.state, onRetry: { model.retry() }) { sections in
				content(sections)
			}
			.navigationTitle("Playlists")
			.navigationDestination(for: Route.self) { route in
				if case .playlist(let ref, let name) = route {
					PlaylistDetailView(ref: ref, playlistName: name)
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
		// A playlist edited from an album three screens away has to show up
		// here without a manual refresh.
		.task(id: events.playlistRevision) { model.appear() }
		.onChange(of: selection.scope) { path.removeAll() }
		.confirmationDialog(
			"Delete \(confirmingDelete?.name ?? "this playlist")?",
			isPresented: Binding(
				get: { confirmingDelete != nil },
				set: { if !$0 { confirmingDelete = nil } }),
			titleVisibility: .visible
		) {
			Button("Delete", role: .destructive) {
				if let playlist = confirmingDelete {
					Task { await model.delete(playlist) }
				}
				confirmingDelete = nil
			}
			Button("Cancel", role: .cancel) { confirmingDelete = nil }
		} message: {
			Text("This deletes the playlist on the server. The tracks are not affected.")
		}
		.alert(
			"Could not delete",
			isPresented: Binding(
				get: { model.error != nil },
				set: { if !$0 { model.clearError() } })
		) {
			Button("OK") { model.clearError() }
		} message: {
			Text(model.error ?? "")
		}
	}

	@ViewBuilder
	private func content(_ sections: [ServerSection<Playlist>]) -> some View {
		if sections.isEmpty {
			EmptyMessage(text: "No playlists")
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
						ForEach(section.items) { playlist in
							NavigationLink(value: Route.playlist(playlist.ref, name: playlist.name)) {
								PlaylistRow(playlist: playlist)
							}
							.swipeActions(edge: .trailing) {
								Button(role: .destructive) {
									confirmingDelete = playlist
								} label: {
									Label("Delete", systemImage: "trash")
								}
							}
						}
					} header: {
						// Suppressed when there is only one section, since a
						// heading naming the only server present says nothing.
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
}
