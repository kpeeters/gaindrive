//	GainDrive for iOS
//	Copyright (C) 2026 Kasper Peeters
//
//	This file is part of GainDrive, distributed under the GNU General
//	Public License version 3 or later, with the Application Distribution
//	Exception in ios/LICENSE. See LICENSE at the repository root for the
//	full text of the GPL.

import SwiftUI

/// The Library tab: the merged listing, one artist's albums, and one album's
/// tracks - or the same for the account's own uploads.
///
/// Laid out by `PaneNavigator`: a stack on a phone or a narrow window, two or
/// three panes side by side from 650 and 900 points. The rows select rather
/// than link, and the path is derived from the two selections, so choosing a
/// row pushes on a phone and fills the next pane on a tablet, and popping or
/// going back clears the selection it came from.
///
/// **Uploads replaces the listing at the root rather than covering the tab.**
/// A full-screen cover hid the sidebar on an iPad and the Mac, which the web
/// client never does; there, Uploads opens inside the layout and the rest of
/// the navigation stays. The same everywhere, a phone included, since one
/// behaviour is simpler than two and the stack treats it like any other root.
struct ArtistsView: View {
	let model: ArtistsViewModel

	@Environment(ServerSelection.self) private var servers
	@Environment(SettingsStore.self) private var settings
	@Environment(\.library) private var library
	@Environment(LibraryEvents.self) private var events
	/// **The selections carry the domain values, not ids.** The albums pane
	/// wants the artist's `refs`, name and the section it was picked from, and
	/// the tracks pane wants the album's title for its bar before the load
	/// returns. Tagging the value means neither has to look anything up.
	@State private var selectedChoice: ArtistChoice?
	@State private var selectedAlbum: Album?

	/// The uploads listing's model while Uploads is showing, nil otherwise.
	/// Built when the icon is tapped and dropped on leaving - Android's
	/// fresh-listing-per-visit lifecycle, and what stops a stale `loadedFor`
	/// surviving a second visit.
	@State private var uploadsModel: ArtistsViewModel?

	/// The levels past the root, as the navigator reads them.
	enum Level: Hashable {
		case artist(ArtistChoice)
		case album(Album)
	}

	var body: some View {
		PaneNavigator(
			path: path, maxLevels: 3,
			root: { root },
			destination: { destination($0) },
			placeholder: { level in
				ContentUnavailableView(
					level == 1 ? "Choose an artist" : "Choose an album",
					systemImage: level == 1 ? "music.mic" : "music.note.list")
			})
			// A different scope is a different library, so what was chosen in
			// the old one is not a screen the user can act on.
			.onChange(of: servers.scope) { clearSelection() }
			// Going offline is a different library, for the same reason.
			.onChange(of: settings.offlineMode) { clearSelection() }
			// A different artist cannot still have the same album showing.
			.onChange(of: selectedChoice) { selectedAlbum = nil }
			// Moved or deleted on the server: the album showing may no longer
			// be where it was, or be at all.
			.onChange(of: events.libraryRevision) { selectedAlbum = nil }
	}

	/// Derived from the selections, and written back into them: a shorter
	/// path is a pop, which clears what was popped.
	private var path: Binding<[Level]> {
		Binding(
			get: {
				guard let choice = selectedChoice else { return [] }
				guard let album = selectedAlbum else { return [.artist(choice)] }
				return [.artist(choice), .album(album)]
			},
			set: { levels in
				var choice: ArtistChoice?
				var album: Album?
				for level in levels {
					switch level {
					case .artist(let picked): choice = picked
					case .album(let picked): album = picked
					}
				}
				if choice != selectedChoice { selectedChoice = choice }
				selectedAlbum = album
			})
	}

	@ViewBuilder
	private var root: some View {
		if let uploadsModel {
			ArtistsList(
				model: uploadsModel, uploads: true, selection: $selectedChoice,
				onOpenUploads: nil, onClose: closeUploads)
		} else {
			ArtistsList(
				model: model, uploads: false, selection: $selectedChoice,
				onOpenUploads: openUploadsAction, onClose: nil)
		}
	}

	@ViewBuilder
	private func destination(_ level: Level) -> some View {
		switch level {
		case .artist(let choice):
			AlbumsView(
				refs: choice.artist.refs, artistName: choice.artist.name,
				fromCategories: choice.section == .categories,
				fromUploads: choice.section == .uploads,
				selection: $selectedAlbum)
		case .album(let album):
			AlbumDetailView(
				ref: album.ref, albumTitle: album.title, fromUploads: uploadsModel != nil)
		}
	}

	/// Typed out of line rather than written as a conditional at the call
	/// site: a `nil` against an unapplied method reference, left to inference
	/// inside the body's builder, pushed the type checker into "failed to
	/// produce diagnostic". A declared type gives it nothing to infer.
	private var openUploadsAction: (() -> Void)? {
		{ openUploads() }
	}

	private func openUploads() {
		guard let library else { return }
		clearSelection()
		uploadsModel = ArtistsViewModel(library: library, selection: servers, uploads: true)
	}

	private func closeUploads() {
		clearSelection()
		uploadsModel = nil
	}

	private func clearSelection() {
		selectedChoice = nil
		selectedAlbum = nil
	}
}

/// A picked row and the section it was picked from. The section travels with
/// the selection because an artist ref says which folder, never which listing
/// it was reached through - and two downstream screens key on that: a
/// category has no portrait or biography, and an upload's album sort has a
/// key of its own. The same reason Android's `onOpenArtist` passes a
/// `LibrarySection` beside the refs.
struct ArtistChoice: Hashable {
	let artist: Artist
	let section: LibrarySection
}

/// The merged listing - categories under one header, then every artist in
/// index buckets with a fast-scroll rail - or the uploads listing.
///
/// The first pane on a wide screen, and the tab's first screen on a phone.
/// `.listStyle(.plain)` stays for that reason: `.sidebar` is the iPad idiom,
/// but this is the same view the phone shows - and the pane it fills is a
/// third of the window, not a sidebar.
private struct ArtistsList: View {
	let model: ArtistsViewModel
	let uploads: Bool
	@Binding var selection: ArtistChoice?
	/// The way into the uploads listing, present only on the library
	/// instance - on the uploads listing the icon would be a door into the
	/// room you are standing in.
	let onOpenUploads: (() -> Void)?
	/// The way back to the library from the uploads listing; nil on the
	/// library itself.
	let onClose: (() -> Void)?

	@Environment(ServerSelection.self) private var servers
	@Environment(SettingsStore.self) private var settings
	@Environment(LibraryEvents.self) private var events
	@Environment(Fetches.self) private var fetches
	@Environment(\.library) private var library
	/// Held in the *view*, not the view model: dismissing a note must not cost
	/// a second fan-out across every server.
	@State private var notesDismissed = false
	@State private var fetching = false

	var body: some View {
		LoadStateBox(state: model.state, onRetry: { model.retry() }) { listing in
			content(listing)
		}
		.navigationTitle(uploads ? "Uploads" : "Library")
		.toolbar {
			// Uploads replaces the listing at the root, so the way back to the
			// library is a button where a back button would be - first.
			if let onClose {
				ToolbarItem(placement: .leadingBar) {
					Button(action: onClose) {
						Label("Library", systemImage: "chevron.backward")
					}
				}
			}
			ToolbarItem(placement: .leadingBar) { LibrarySelector() }
			if uploads {
				// Where Android and the web put the fetch panel: in the
				// uploads, which is where a fetch lands.
				ToolbarItem(placement: .trailingBar) {
					Button {
						fetching = true
					} label: {
						Label("Fetch from a URL", systemImage: "link.badge.plus")
					}
				}
			}
			if let onOpenUploads, model.canUpload {
				ToolbarItem(placement: .trailingBar) {
					Button(action: onOpenUploads) {
						// Not `square.and.arrow.up`, which is the share glyph
						// and would read as "share this screen": this is the
						// Android app's Upload icon - putting something into
						// a holding area.
						Label("Uploads", systemImage: "tray.and.arrow.up")
					}
				}
			}
			// Unconditional rather than iOS-only: Mac Catalyst has
			// `.refreshable` but no gesture that comfortably reaches it, so
			// without this the Catalyst build has no way to reload at all.
			ToolbarItem(placement: .trailingBar) {
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
		// An upload moved or deleted, or a fetch landed. Kept on screen while
		// it re-reads, as a pull does.
		.onChange(of: events.libraryRevision) { Task { await model.refresh() } }
		// Handed over explicitly: on the Mac, sheets raised from tab content
		// have been presented without their environment.
		.sheet(isPresented: $fetching) {
			FetchUrlView()
				.environment(fetches)
				.environment(settings)
				.environment(\.library, library)
		}
	}

	@ViewBuilder
	private func content(_ listing: LibraryListing) -> some View {
		if listing.isEmpty {
			EmptyMessage(text: emptyText)
		} else {
			ScrollViewReader { proxy in
				List(selection: $selection) {
					// Untagged, and so not selectable - which is what keeps a
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
					// The whole group under one heading - a library holds a
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
		// Offline, an empty list is not a library with no artists in it - it is
		// a library with nothing downloaded. "No artists" would send somebody
		// looking for a server problem that is not there.
		if settings.offlineMode { return "Nothing downloaded yet" }
		return uploads ? "Nothing in your uploads yet" : "No artists"
	}

	/// The rail is for scrubbing a long alphabetical list, not for jumping
	/// between four people. An admin's uploads listing comes back bucketed by
	/// **username**, and a vertical strip of those reads as a mistake. It
	/// scans the artist buckets only - the Categories header above them is
	/// not a stop.
	private func showsRail(_ indexes: [ArtistIndex]) -> Bool {
		indexes.count > 1 && indexes.allSatisfy { $0.label.count == 1 }
	}
}
