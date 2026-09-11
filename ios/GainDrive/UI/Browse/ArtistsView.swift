//	GainDrive for iOS
//	Copyright (C) 2026 Kasper Peeters
//
//	This file is part of GainDrive, distributed under the GNU General
//	Public License version 3 or later, with the Application Distribution
//	Exception in ios/LICENSE. See LICENSE at the repository root for the
//	full text of the GPL.

import SwiftUI

/// The Library tab: the merged listing, one artist's albums, and one album's
/// tracks — and, presented over it, the same three panes a second time for
/// the account's own uploads.
///
/// **The only tab with three panes**, and that is a decision rather than an
/// accident. It is the one with three genuine levels, which is exactly three
/// panes' worth. Playlists and Recents are two levels deep and would gain a
/// pane that mostly stood empty; Search keeps its results whatever the width,
/// which a pane layout cannot express. Those three keep their
/// `NavigationStack`s, and this one no longer has one.
///
/// **Regular width draws the three panes by hand; compact keeps a
/// `NavigationSplitView`.** A three-column split view drew the wide layout
/// once, but nested inside the `.sidebarAdaptable` `TabView` its automatic
/// style resolves to prominent-detail behaviour: the artists column becomes a
/// floating overlay that hides the albums column, and the visible columns get
/// unequal, width-dependent sizes. An `HStack` of three equal panes has no
/// such state machine. The split view survives on compact for the one thing
/// it did reliably — collapsing to a push stack driven by the same
/// `List(selection:)` bindings — so the iPhone behaves as it always did,
/// which is the thing most worth checking.
struct ArtistsView: View {
	let model: ArtistsViewModel
	/// The uploads flavour: same screen, same panes, the personal listing.
	/// A parameter rather than module state, so the two instances cannot
	/// share or leak a selection.
	var uploads = false

	@Environment(ServerSelection.self) private var servers
	@Environment(SettingsStore.self) private var settings
	@Environment(\.library) private var library
	@Environment(\.horizontalSizeClass) private var sizeClass
	/// **The selections carry the domain values, not ids.** A `List` would
	/// default to the element's `id`, which here is an `ItemRef` — and both
	/// trailing panes need more than that: the albums pane wants the
	/// artist's `refs`, name and the section the row was picked from, and the
	/// tracks pane wants the album's title for its bar before the load
	/// returns. Tagging the value means neither has to look anything up, and
	/// there is no stale id to resolve against a list that has since reloaded.
	@State private var selectedChoice: ArtistChoice?
	@State private var selectedAlbum: Album?

	/// The uploads listing's own model, built when the icon is tapped and
	/// discarded on dismiss — Android's fresh-listing-per-visit lifecycle,
	/// and what stops a stale `loadedFor` surviving a re-presentation.
	@State private var uploadsModel: ArtistsViewModel?
	@State private var showingUploads = false

	var body: some View {
		layout
			// Replaces what a `NavigationStack` path did here: a different scope
			// is a different library, so what was chosen in the old one is not a
			// screen the user can act on.
			.onChange(of: servers.scope) { clearSelection() }
			// Going offline is a different library, so what was chosen in the other
			// one is not a screen the user can act on.
			.onChange(of: settings.offlineMode) { clearSelection() }
			// A different artist cannot still have the same album showing.
			.onChange(of: selectedChoice) { selectedAlbum = nil }
			// A cover rather than a sheet: this hosts a second pane layout, which
			// needs the full width on an iPad — a centred sheet card cannot give
			// it three panes — and collapses to a stack on a phone exactly like
			// the view underneath. Presented from within the tab's content, so it
			// inherits the environment; the caveat in RootView about sheets losing
			// it applies only to presentations hung off the TabView itself.
			.fullScreenCover(isPresented: $showingUploads, onDismiss: { uploadsModel = nil }) {
				uploadsCover
			}
	}

	/// The branch, with everything cross-cutting hung on the container above
	/// it: the clearing rules and the uploads cover must survive a size-class
	/// flip (rotation into a multitasking split, a Catalyst window resize)
	/// rather than being torn down with the branch they happened to sit on.
	/// The selections live above it too, so a flip keeps them; the pane view
	/// models rebuild and refetch, which is the same cost `.id()` already
	/// pays on every change of artist.
	@ViewBuilder
	private var layout: some View {
		if sizeClass == .regular {
			panes
		} else {
			splitView
		}
	}

	/// Three equal panes, by hand. One `NavigationStack` *per pane*, so each
	/// pane hosts exactly the bar its view already declares — which is what
	/// leaves `ArtistsList`, `AlbumsView` and `AlbumDetailView` untouched,
	/// and with them the phone path. Nothing ever pushes: a non-nil selection
	/// binding makes `AlbumsView` render tagged rows rather than links, so
	/// the stacks are pure chrome hosts.
	private var panes: some View {
		HStack(spacing: 0) {
			pane {
				ArtistsList(
					model: model, uploads: uploads, selection: $selectedChoice,
					onOpenUploads: openUploadsAction)
				// Inline like its neighbours: a large title beside two inline
				// bars is three bars of two heights.
				.navigationBarTitleDisplayMode(.inline)
			}
			Divider()
			// The empty titles keep an (empty, inline) bar over the
			// placeholder branches, so the bars stay one height before
			// anything is selected; a deeper `.navigationTitle` wins the
			// moment a selection exists.
			pane { albums.navigationTitle("") }
			Divider()
			pane { tracks.navigationTitle("") }
		}
	}

	/// Out of line for the same reason as `openUploadsAction`: the smaller
	/// each expression the body has to solve, the better.
	///
	/// Clipped, because a pane flush against a safe-area edge is extended
	/// into it by SwiftUI's full-bleed rule for scrollables — the artists
	/// list's selection highlight drew under (and in the floating gap left
	/// of) the TabView sidebar, through its translucent material. Row
	/// content was already safe-area inset; only the background leaked, so
	/// the clip moves nothing. In the helper so all three panes get it: the
	/// trailing pane has the mirror-image bleed on the right, with nothing
	/// tinted to show it.
	private func pane(@ViewBuilder _ content: () -> some View) -> some View {
		NavigationStack { content() }
			.frame(maxWidth: .infinity)
			.clipped()
	}

	/// Compact only, kept for the one thing the split view does reliably:
	/// collapsing to a push stack derived from the live selections, so
	/// shrinking into a multitasking split lands on the screen that was
	/// showing. A hand-built `NavigationStack` path would need pops
	/// synchronised back to the selection clearing — exactly the identity-bug
	/// class the `.id()` comments below warn about. No visibility binding:
	/// compact ignores it.
	private var splitView: some View {
		NavigationSplitView {
			ArtistsList(
				model: model, uploads: uploads, selection: $selectedChoice,
				onOpenUploads: openUploadsAction)
		} content: {
			albums
		} detail: {
			tracks
		}
	}

	/// Typed out of line rather than written as `uploads ? nil : openUploads`
	/// at the call site: a `nil` against an unapplied method reference, with
	/// the optional-closure type left to inference inside the body's builder,
	/// is what pushed the type checker into "failed to produce diagnostic".
	/// A property with a declared type gives the solver nothing to infer.
	private var openUploadsAction: (() -> Void)? {
		if uploads { return nil }
		return { openUploads() }
	}

	/// Out of the body for the same reason as `openUploadsAction`: the
	/// smaller each expression the body has to solve, the better.
	@ViewBuilder
	private var uploadsCover: some View {
		if let uploadsModel {
			ArtistsView(model: uploadsModel, uploads: true)
		}
	}

	private func openUploads() {
		guard let library else { return }
		// Built here rather than held ready: initialisers do no work, and a
		// model that exists only while its screen does cannot go stale.
		uploadsModel = ArtistsViewModel(library: library, selection: servers, uploads: true)
		showingUploads = true
	}

	@ViewBuilder
	private var albums: some View {
		if let choice = selectedChoice {
			AlbumsView(
				refs: choice.artist.refs, artistName: choice.artist.name,
				fromCategories: choice.section == .categories,
				fromUploads: choice.section == .uploads,
				selection: $selectedAlbum
			)
			// **A fresh identity per artist.** `AlbumsView` builds its view
			// model once and keeps it in `@State`, so without this SwiftUI
			// reuses the view across a change of artist and it goes on showing
			// the previous one's albums — which reads as a stale list rather
			// than as an error, and is the failure here most likely to be
			// missed.
			.id(choice.artist.ref)
		} else {
			// One line rather than a blank pane. `android/SCREENS.md`: the
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
		selectedChoice = nil
		selectedAlbum = nil
	}
}

/// A picked row and the section it was picked from. The section travels with
/// the selection because an artist ref says which folder, never which listing
/// it was reached through — and two downstream screens key on that: a
/// category has no portrait or biography, and an upload's album sort has a
/// key of its own. The same reason Android's `onOpenArtist` passes a
/// `LibrarySection` beside the refs.
struct ArtistChoice: Hashable {
	let artist: Artist
	let section: LibrarySection
}

/// The merged listing — categories under one header, then every artist in
/// index buckets with a fast-scroll rail — or the uploads listing.
///
/// The first pane on a wide screen, and the tab's first screen on a phone.
/// `.listStyle(.plain)` stays for that reason: `.sidebar` is the iPad idiom,
/// but this is the same view the phone shows — and the pane it fills is a
/// third of the window, not a sidebar.
private struct ArtistsList: View {
	let model: ArtistsViewModel
	let uploads: Bool
	@Binding var selection: ArtistChoice?
	/// The way into the uploads listing, present only on the library
	/// instance — on the uploads listing the icon would be a door into the
	/// room you are standing in.
	let onOpenUploads: (() -> Void)?

	@Environment(ServerSelection.self) private var servers
	@Environment(SettingsStore.self) private var settings
	@Environment(\.dismiss) private var dismiss
	/// Held in the *view*, not the view model: dismissing a note must not cost
	/// a second fan-out across every server.
	@State private var notesDismissed = false

	var body: some View {
		LoadStateBox(state: model.state, onRetry: { model.retry() }) { listing in
			content(listing)
		}
		.navigationTitle(uploads ? "Uploads" : "Library")
		.toolbar {
			ToolbarItem(placement: .topBarLeading) { LibrarySelector() }
			// A cover has no back gesture of its own on every platform this
			// runs on; Done is the way out, in the slot iOS reserves for it.
			if uploads {
				ToolbarItem(placement: .cancellationAction) {
					Button("Done") { dismiss() }
				}
			}
			if let onOpenUploads, model.canUpload {
				ToolbarItem(placement: .topBarTrailing) {
					Button(action: onOpenUploads) {
						// Not `square.and.arrow.up`, which is the share glyph
						// and would read as "share this screen": this is the
						// Android app's Upload icon — putting something into
						// a holding area.
						Label("Uploads", systemImage: "tray.and.arrow.up")
					}
				}
			}
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
		// The listing is a different one offline, and nothing else would ask
		// for it: `appear` is keyed on the scope, which has not changed.
		.onChange(of: settings.offlineMode) { model.retry() }
	}

	@ViewBuilder
	private func content(_ listing: LibraryListing) -> some View {
		if listing.isEmpty {
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
					// The whole group under one heading — a library holds a
					// handful of sections, not enough to bucket by letter.
					// The header carries no `.id`, so it is not a rail stop;
					// the rail belongs to the artist buckets below.
					if !listing.categories.isEmpty {
						Section {
							ForEach(listing.categories) { artist in
								ArtistRow(item: model.artistUi(artist))
									.tag(ArtistChoice(artist: artist, section: .categories))
							}
						} header: {
							Text("Categories").pinnedHeaderBackground()
						}
					}
					ForEach(listing.artists) { bucket in
						Section {
							ForEach(bucket.artists) { artist in
								ArtistRow(item: model.artistUi(artist))
									.tag(
										ArtistChoice(
											artist: artist,
											section: uploads ? .uploads : .artists))
							}
						} header: {
							Text(bucket.label).id(bucket.label).pinnedHeaderBackground()
						}
					}
				}
				.listStyle(.plain)
				.refreshable { await model.refresh() }
				// Long names would otherwise slide underneath the rail rather
				// than being clipped short of it.
				.safeAreaPadding(.trailing, showsRail(listing.artists) ? 20 : 0)
				.overlay(alignment: .trailing) {
					if showsRail(listing.artists) {
						AlphabetRail(labels: listing.artists.map(\.label)) { label in
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

	/// "No artists" would read as a fault in the uploads listing, where an
	/// empty list is the ordinary state of somewhere nothing has been put yet.
	private var emptyText: String {
		if servers.hasNoServers { return "No servers configured" }
		// Offline, an empty list is not a library with no artists in it — it is
		// a library with nothing downloaded. "No artists" would send somebody
		// looking for a server problem that is not there.
		if settings.offlineMode { return "Nothing downloaded yet" }
		return uploads ? "Nothing in your uploads yet" : "No artists"
	}

	/// The rail is for scrubbing a long alphabetical list, not for jumping
	/// between four people. An admin's uploads listing comes back bucketed by
	/// **username**, and a vertical strip of those reads as a mistake. It
	/// scans the artist buckets only — the Categories header above them is
	/// not a stop.
	private func showsRail(_ indexes: [ArtistIndex]) -> Bool {
		indexes.count > 1 && indexes.allSatisfy { $0.label.count == 1 }
	}
}
