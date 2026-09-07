//	GainDrive for iOS
//	Copyright (C) 2026 Kasper Peeters
//
//	This file is part of GainDrive, distributed under the GNU General
//	Public License version 3 or later, with the Application Distribution
//	Exception in ios/LICENSE. See LICENSE at the repository root for the
//	full text of the GPL.

import SwiftUI

/// The start destination: every artist across the current scope, in index
/// buckets with a fast-scroll rail.
struct ArtistsView: View {
	let model: ArtistsViewModel

	@Environment(ServerSelection.self) private var selection
	@State private var path: [Route] = []
	/// Held in the *view*, not the view model: dismissing a note must not cost
	/// a second fan-out across every server.
	@State private var notesDismissed = false

	var body: some View {
		NavigationStack(path: $path) {
			LoadStateBox(state: model.state, onRetry: { model.retry() }) { indexes in
				content(indexes)
			}
			// Outside the list rather than a row in it, so the chips are there
			// while the slice is loading, when the load failed, and when there
			// is nothing in it yet — which is exactly when somebody wants to be
			// somewhere else.
			.safeAreaInset(edge: .top, spacing: 0) {
				LibraryModeChips(
					modes: model.modes, selected: model.mode,
					onSelect: { model.select($0) })
			}
			.navigationTitle("Artists")
			.navigationDestination(for: Route.self) { route in
				destination(route)
			}
			.toolbar {
				ToolbarItem(placement: .topBarLeading) { LibrarySelector() }
				// Unconditional rather than iOS-only: Mac Catalyst has
				// `.refreshable` but no gesture that comfortably reaches it, so
				// without this the Catalyst build has no way to reload at all.
				ToolbarItem(placement: .topBarTrailing) {
					Button {
						Task { await model.refresh() }
					} label: {
						Label("Refresh", systemImage: "arrow.clockwise")
					}
				}
			}
		}
		.task(id: selection.scope) {
			notesDismissed = false
			model.appear()
		}
		// Changing scope pops any deeper navigation that no longer applies —
		// an album on a server that is no longer in scope is not a screen the
		// user can act on.
		.onChange(of: selection.scope) { path.removeAll() }
	}

	@ViewBuilder
	private func content(_ indexes: [ArtistIndex]) -> some View {
		if indexes.isEmpty {
			EmptyMessage(text: emptyText)
		} else {
			ScrollViewReader { proxy in
				List {
					if !model.failures.isEmpty, !notesDismissed {
						PartialFailureNote(
							failures: model.failures,
							onRetry: { Task { await model.refresh() } },
							onDismiss: { notesDismissed = true }
						)
						.listRowSeparator(.hidden)
					}
					ForEach(indexes) { bucket in
						Section {
							ForEach(bucket.artists) { artist in
								NavigationLink(
									value: Route.albums(
										artists: artist.refs, name: artist.name,
										fromCategories: model.mode == .categories)
								) {
									ArtistRow(item: model.artistUi(artist))
								}
							}
						} header: {
							Text(bucket.label).id(bucket.label)
						}
					}
				}
				.listStyle(.plain)
				.refreshable { await model.refresh() }
				// Long names would otherwise slide underneath the rail rather
				// than being clipped short of it.
				.safeAreaPadding(.trailing, showsRail(indexes) ? 20 : 0)
				.overlay(alignment: .trailing) {
					if showsRail(indexes) {
						AlphabetRail(labels: indexes.map(\.label)) { label in
							// Not animated: scrubbing the rail issues these in
							// quick succession, and animations queue up and lag
							// behind the finger.
							proxy.scrollTo(label, anchor: .top)
						}
					}
				}
			}
		}
	}

	/// "No artists" would read as a fault in the uploads slice, where an empty
	/// list is the ordinary state of somewhere nothing has been put yet.
	private var emptyText: String {
		if selection.hasNoServers { return "No servers configured" }
		return model.mode == .uploads ? "Nothing in your uploads yet" : "No artists"
	}

	/// The rail is for scrubbing a long alphabetical list, not for jumping
	/// between four people. An admin's Uploads slice comes back bucketed by
	/// **username**, and a vertical strip of those reads as a mistake.
	private func showsRail(_ indexes: [ArtistIndex]) -> Bool {
		indexes.count > 1 && indexes.allSatisfy { $0.label.count == 1 }
	}

	@ViewBuilder
	private func destination(_ route: Route) -> some View {
		switch route {
		case .albums(let artists, let name, let fromCategories):
			AlbumsView(refs: artists, artistName: name, fromCategories: fromCategories)
		case .album(let ref, let title, let autoPlay):
			AlbumDetailView(ref: ref, albumTitle: title, autoPlay: autoPlay)
		case .playlist:
			// Playlists are reached from their own tab; this arm exists so the
			// switch is exhaustive rather than because it can happen.
			EmptyMessage(text: "Not available here")
		}
	}
}
