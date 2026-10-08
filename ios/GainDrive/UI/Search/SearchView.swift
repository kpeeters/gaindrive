//	GainDrive for iOS
//	Copyright (C) 2026 Kasper Peeters
//
//	This file is part of GainDrive, distributed under the GNU General
//	Public License version 3 or later, with the Application Distribution
//	Exception in ios/LICENSE. See LICENSE at the repository root for the
//	full text of the GPL.

import SwiftUI
#if os(macOS)
	import AppKit
	import os
#endif

struct SearchView: View {
	@Bindable var model: SearchViewModel

	@Environment(ServerSelection.self) private var selection
	@State private var path: [Route] = []
	@State private var notesDismissed = false
	/// The album picked in an artist's pane; see `Route.albumSelection`.
	@State private var chosenAlbum: Album?
	#if os(macOS)
		@Environment(\.paneHeaderLeadingInset) private var leadingInset
		// TEMPORARY: diagnosing the field losing focus once results are listed.
		@FocusState private var fieldFocused: Bool
		private static let log = Logger(subsystem: "org.gaindrive.ios-player", category: "search")
	#endif

	var body: some View {
		#if os(macOS)
			let _ = Self._printChanges()
		#endif
		// Results stay in the first pane whatever is opened beside them, which
		// is what a search wants: the next hit is one tap away. Three levels:
		// results, an artist, one of their albums.
		PaneNavigator(
			path: $path, maxLevels: 3,
			root: {
				searchRoot
			},
			destination: { destination($0) },
			placeholder: { _ in
				ContentUnavailableView("Choose a result", systemImage: "magnifyingglass")
			},
			rootDrawsHeader: true
		)
		.task(id: selection.scope) { model.scopeChanged() }
		.onChange(of: selection.scope) { path.removeAll() }
		// A track link: replaces whatever was pushed, since what was asked
		// for is this album and not a detour behind another one.
		.onChange(of: model.pendingRoute, initial: true) {
			guard let route = model.pendingRoute else { return }
			path = [route]
			model.consumePendingRoute()
		}
	}

	/// The results pane. On iOS the field is `.searchable`, in the results
	/// pane's bar. On the Mac `.searchable` would put it in the window's
	/// toolbar, above every pane, so there the field is this pane's whole
	/// header, drawn here rather than declared with `.paneHeader`.
	///
	/// A sibling above the content, not an inset or overlay on it: attached
	/// to the content, the field was re-created each time the content
	/// switched (searching, results) and on every focus change, so it lost
	/// focus at once. The content is stretched because a
	/// `ContentUnavailableView` is only as tall as its text.
	private var searchRoot: some View {
		#if os(macOS)
			VStack(spacing: 0) {
				TextField("Artists, albums and tracks", text: $model.query)
					.textFieldStyle(.roundedBorder)
					.focused($fieldFocused)
					// TEMPORARY: re-creation shows as an appear/disappear
					// pair; a theft as focus false with someone else's
					// responder.
					.onAppear { Self.log.notice("field appeared") }
					.onDisappear { Self.log.notice("field disappeared") }
					.onChange(of: fieldFocused) { _, focused in
						Self.log.notice("field focused: \(focused)")
						guard !focused else { return }
						DispatchQueue.main.async {
							let responder = NSApp.keyWindow?.firstResponder
							Self.log.notice(
								"first responder now: \(String(describing: responder), privacy: .public)")
						}
					}
					// TEMPORARY experiment: plain header without the `.bar`
					// material and window drag of `paneHeaderChrome`.
					.padding(.horizontal, 12)
					.padding(.leading, leadingInset)
					.frame(height: PaneHeaderBar.height)
					.frame(maxWidth: .infinity)
					.background(Color(nsColor: .windowBackgroundColor))
				Divider()
				content
					.frame(maxWidth: .infinity, maxHeight: .infinity)
					// TEMPORARY: if this pairs up too, the whole pane is
					// being rebuilt from above.
					.onAppear { Self.log.notice("content appeared") }
					.onDisappear { Self.log.notice("content disappeared") }
			}
		#else
			content
				.paneHeader("Search") {
					LibrarySelector()
				} actions: {
					EmptyView()
				}
				.searchable(text: $model.query, prompt: "Artists, albums and tracks")
		#endif
	}

	@ViewBuilder
	private var content: some View {
		switch model.phase {
		case .idle:
			ContentUnavailableView(
				"Search your library", systemImage: "magnifyingglass",
				description: Text("Results appear as each server answers."))
		case .searching:
			ProgressView().frame(maxWidth: .infinity, maxHeight: .infinity)
		case .failed(let message):
			ContentUnavailableView {
				Label("Search failed", systemImage: "exclamationmark.triangle")
			} description: {
				Text(message)
			}
		case .ready(let results):
			if results.isEmpty, !results.outstanding {
				ContentUnavailableView.search(text: model.query)
			} else {
				list(results)
			}
		}
	}

	private func list(_ results: SearchResults) -> some View {
		List(selection: PaneSelection.selection($path, level: 0)) {
			filterChips

			if !results.failures.isEmpty, !notesDismissed {
				PartialFailureNote(
					failures: results.failures,
					onRetry: { model.retry() },
					onDismiss: { notesDismissed = true }
				)
				.listRowSeparator(.hidden)
			}

			if !results.artists.isEmpty {
				Section {
					ForEach(results.artists) { item in
						ArtistRow(item: item)
							.tag(Route.albums(artists: item.artist.refs, name: item.artist.name))
					}
				} header: {
					SectionHeading(text: "Artists").pinnedHeaderBackground()
				}
			}

			if !results.albums.isEmpty {
				Section {
					ForEach(results.albums) { item in
						AlbumRow(item: item)
							.tag(Route.album(item.album.ref, title: item.album.title))
					}
				} header: {
					SectionHeading(text: "Albums").pinnedHeaderBackground()
				}
			}

			if !results.songs.isEmpty {
				Section {
					ForEach(results.songs) { item in
						songRow(item)
					}
				} header: {
					SectionHeading(text: "Tracks").pinnedHeaderBackground()
				}
			}

			// Last, and a section of their own. A marker is the least likely
			// thing somebody was searching for, and folding these in among the
			// tracks would offer rows that cannot be starred, queued or
			// downloaded beside rows that can.
			if !results.chapters.isEmpty {
				Section {
					ForEach(results.chapters) { hit in
						chapterRow(hit)
					}
				} header: {
					SectionHeading(text: "Chapters").pinnedHeaderBackground()
				}
			}
		}
		.listStyle(.plain)
		// Quiet rather than blocking: what has arrived is already usable, and
		// a spinner over it would hide the fast server's answer while the slow
		// one is still thinking.
		.overlay(alignment: .top) {
			if results.outstanding {
				ProgressView().progressViewStyle(.linear)
			}
		}
	}

	/// As in Recents: a hit opens its album and starts there rather than
	/// playing in place. `Route.album` records why, and it is not cosmetic -
	/// a video hit played where it stands is re-encoded rather than remuxed.
	@ViewBuilder
	private func songRow(_ item: SongUi) -> some View {
		if let album = item.song.albumRef {
			SongRow(item: item)
				.tag(Route.album(album, title: item.song.albumTitle, autoPlay: item.song.ref))
				.trackActions(for: item.song)
		} else {
			SongRow(item: item)
				.trackActions(for: item.song)
		}
	}

	/// A marker cannot be played where it stands - it has no id to stream - so
	/// this opens the album its recording sits in and starts that recording at
	/// the marker. The same detour `songRow` takes, and it buys the same thing
	/// besides: read again through `getAlbum`, the recording carries
	/// `nativeSeek`, so a remuxable concert is not re-encoded to reach one song.
	///
	/// A hit whose server did not name the album folder is drawn and inert.
	/// There is nowhere to send it, and a row that navigated nowhere on tap
	/// would be worse than one that plainly does not.
	@ViewBuilder
	private func chapterRow(_ hit: ChapterHit) -> some View {
		if let album = hit.albumRef {
			ChapterHitRow(hit: hit)
				.tag(
					Route.album(
						album, title: hit.albumTitle, autoPlay: hit.songRef, autoPlayAt: hit.start))
		} else {
			ChapterHitRow(hit: hit)
		}
	}

	private var filterChips: some View {
		HStack(spacing: 8) {
			Toggle("Artists", isOn: $model.filters.artists)
			Toggle("Albums", isOn: $model.filters.albums)
			Toggle("Tracks", isOn: $model.filters.songs)
		}
		.toggleStyle(.button)
		.buttonStyle(.bordered)
		.controlSize(.small)
		.listRowSeparator(.hidden)
	}

	@ViewBuilder
	private func destination(_ route: Route) -> some View {
		switch route {
		case .albums(let artists, let name, let fromCategories, let fromUploads):
			// Only ever at level 1, opened from a result, so the album it
			// opens is level 2.
			AlbumsView(
				refs: artists, artistName: name,
				fromCategories: fromCategories, fromUploads: fromUploads,
				selection: Route.albumSelection($path, level: 1, chosen: $chosenAlbum))
		case .album(let ref, let title, let autoPlay, let at):
			AlbumDetailView(ref: ref, albumTitle: title, autoPlay: autoPlay, autoPlayAt: at)
		case .playlist(let ref, let name):
			PlaylistDetailView(ref: ref, playlistName: name)
		}
	}
}
