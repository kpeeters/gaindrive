//	GainDrive for iOS
//	Copyright (C) 2026 Kasper Peeters
//
//	This file is part of GainDrive, distributed under the GNU General
//	Public License version 3 or later, with the Application Distribution
//	Exception in ios/LICENSE. See LICENSE at the repository root for the
//	full text of the GPL.

import SwiftUI

/// The Library tab: artists, one artist's albums, and one album's tracks.
///
/// **The only tab that is a `NavigationSplitView`**, and that is a decision
/// rather than an accident. It is the one with three genuine levels, which is
/// exactly the three columns the API offers. Playlists and Recents are two
/// levels deep and would gain a column that mostly stood empty; Search keeps
/// its results whatever the width, which a split view cannot express. Those
/// three keep their `NavigationStack`s, and this one no longer has one.
///
/// **The collapse to a stack on a phone is SwiftUI's, not ours.** A
/// three-column split view driven by `List(selection:)` becomes a push stack in
/// a compact size class, so the iPhone behaves as it did before this existed —
/// which is the thing most worth checking, because getting it wrong is what
/// would make the change not worth having.
struct ArtistsView: View {
	let model: ArtistsViewModel

	@Environment(ServerSelection.self) private var servers
	@State private var columns = NavigationSplitViewVisibility.all
	/// **The selections carry the domain values, not ids.** A `List` would
	/// default to the element's `id`, which here is an `ItemRef` — and both
	/// trailing columns need more than that: the albums column wants the
	/// artist's `refs` and name, and the tracks column wants the album's title
	/// for its bar before the load returns. Tagging the value means neither has
	/// to look anything up, and there is no stale id to resolve against a list
	/// that has since reloaded.
	@State private var selectedArtist: Artist?
	@State private var selectedAlbum: Album?

	var body: some View {
		NavigationSplitView(columnVisibility: $columns) {
			ArtistsList(model: model, selection: $selectedArtist)
		} content: {
			albums
		} detail: {
			tracks
		}
		// Replaces what a `NavigationStack` path did here: a scope or a slice
		// is a different library, so what was chosen in the old one is not a
		// screen the user can act on.
		.onChange(of: servers.scope) { clearSelection() }
		.onChange(of: model.mode) { clearSelection() }
		// A different artist cannot still have the same album showing.
		.onChange(of: selectedArtist) { selectedAlbum = nil }
	}

	@ViewBuilder
	private var albums: some View {
		if let artist = selectedArtist {
			AlbumsView(
				refs: artist.refs, artistName: artist.name,
				fromCategories: model.mode == .categories,
				selection: $selectedAlbum
			)
			// **A fresh identity per artist.** `AlbumsView` builds its view
			// model once and keeps it in `@State`, so without this SwiftUI
			// reuses the view across a change of artist and it goes on showing
			// the previous one's albums — which reads as a stale list rather
			// than as an error, and is the failure here most likely to be
			// missed.
			.id(artist.ref)
		} else {
			// One line rather than a blank column. `android/SCREENS.md`: the
			// web client leaves it empty, which is fine for a `<div>` and
			// reads as a rendering fault on a tablet.
			ContentUnavailableView("Choose an artist", systemImage: "music.mic")
		}
	}

	@ViewBuilder
	private var tracks: some View {
		if let album = selectedAlbum {
			AlbumDetailView(ref: album.ref, albumTitle: album.title)
				.id(album.ref)
		} else {
			ContentUnavailableView("Choose an album", systemImage: "music.note.list")
		}
	}

	private func clearSelection() {
		selectedArtist = nil
		selectedAlbum = nil
	}
}

/// Every artist across the current scope, in index buckets with a fast-scroll
/// rail.
///
/// The split view's leading column, and — once that collapses — the tab's first
/// screen on a phone. `.listStyle(.plain)` stays for that reason: `.sidebar` is
/// the iPad idiom, but this is the same view the phone shows.
private struct ArtistsList: View {
	let model: ArtistsViewModel
	@Binding var selection: Artist?

	@Environment(ServerSelection.self) private var servers
	/// Held in the *view*, not the view model: dismissing a note must not cost
	/// a second fan-out across every server.
	@State private var notesDismissed = false

	var body: some View {
		LoadStateBox(state: model.state, onRetry: { model.retry() }) { indexes in
			content(indexes)
		}
		// Outside the list rather than a row in it, so the chips are there
		// while the slice is loading, when the load failed, and when there is
		// nothing in it yet — which is exactly when somebody wants to be
		// somewhere else.
		.safeAreaInset(edge: .top, spacing: 0) {
			LibraryModeChips(
				modes: model.modes, selected: model.mode,
				onSelect: { model.select($0) })
		}
		.navigationTitle("Artists")
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
		.task(id: servers.scope) {
			notesDismissed = false
			model.appear()
		}
	}

	@ViewBuilder
	private func content(_ indexes: [ArtistIndex]) -> some View {
		if indexes.isEmpty {
			EmptyMessage(text: emptyText)
		} else {
			ScrollViewReader { proxy in
				List(selection: $selection) {
					// Untagged, and so not selectable — which is what keeps a
					// dismissible note out of the selection model without it
					// having to know there is one.
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
								ArtistRow(item: model.artistUi(artist))
									.tag(artist)
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
		if servers.hasNoServers { return "No servers configured" }
		return model.mode == .uploads ? "Nothing in your uploads yet" : "No artists"
	}

	/// The rail is for scrubbing a long alphabetical list, not for jumping
	/// between four people. An admin's Uploads slice comes back bucketed by
	/// **username**, and a vertical strip of those reads as a mistake.
	private func showsRail(_ indexes: [ArtistIndex]) -> Bool {
		indexes.count > 1 && indexes.allSatisfy { $0.label.count == 1 }
	}
}
