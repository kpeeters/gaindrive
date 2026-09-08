//	GainDrive for iOS
//	Copyright (C) 2026 Kasper Peeters
//
//	This file is part of GainDrive, distributed under the GNU General
//	Public License version 3 or later, with the Application Distribution
//	Exception in ios/LICENSE. See LICENSE at the repository root for the
//	full text of the GPL.

import SwiftUI

struct SearchView: View {
	@Bindable var model: SearchViewModel

	@Environment(ServerSelection.self) private var selection
	@State private var path: [Route] = []
	@State private var notesDismissed = false

	var body: some View {
		NavigationStack(path: $path) {
			content
				.navigationTitle("Search")
				.navigationDestination(for: Route.self) { route in
					destination(route)
				}
				.toolbar {
					ToolbarItem(placement: .topBarLeading) { LibrarySelector() }
				}
		}
		.searchable(text: $model.query, prompt: "Artists, albums and tracks")
		.task(id: selection.scope) { model.scopeChanged() }
		.onChange(of: selection.scope) { path.removeAll() }
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
		List {
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
						NavigationLink(
							value: Route.albums(artists: item.artist.refs, name: item.artist.name)
						) {
							ArtistRow(item: item)
						}
					}
				} header: {
					SectionHeading(text: "Artists")
				}
			}

			if !results.albums.isEmpty {
				Section {
					ForEach(results.albums) { item in
						NavigationLink(value: Route.album(item.album.ref, title: item.album.title)) {
							AlbumRow(item: item)
						}
					}
				} header: {
					SectionHeading(text: "Albums")
				}
			}

			if !results.songs.isEmpty {
				Section {
					ForEach(results.songs) { item in
						songRow(item)
					}
				} header: {
					SectionHeading(text: "Tracks")
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
					SectionHeading(text: "Chapters")
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
	/// playing in place. `Route.album` records why, and it is not cosmetic —
	/// a video hit played where it stands is re-encoded rather than remuxed.
	@ViewBuilder
	private func songRow(_ item: SongUi) -> some View {
		if let album = item.song.albumRef {
			NavigationLink(
				value: Route.album(
					album, title: item.song.albumTitle, autoPlay: item.song.ref)
			) {
				SongRow(item: item)
			}
			.trackActions(for: item.song)
		} else {
			SongRow(item: item)
				.trackActions(for: item.song)
		}
	}

	/// A marker cannot be played where it stands — it has no id to stream — so
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
			NavigationLink(
				value: Route.album(
					album, title: hit.albumTitle, autoPlay: hit.songRef, autoPlayAt: hit.start)
			) {
				ChapterHitRow(hit: hit)
			}
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
		case .albums(let artists, let name, let fromCategories):
			AlbumsView(refs: artists, artistName: name, fromCategories: fromCategories)
		case .album(let ref, let title, let autoPlay, let at):
			AlbumDetailView(ref: ref, albumTitle: title, autoPlay: autoPlay, autoPlayAt: at)
		case .playlist(let ref, let name):
			PlaylistDetailView(ref: ref, playlistName: name)
		}
	}
}
