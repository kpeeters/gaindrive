//	GainDrive for iOS
//	Copyright (C) 2026 Kasper Peeters
//
//	This file is part of GainDrive, distributed under the GNU General
//	Public License version 3 or later, with the Application Distribution
//	Exception in ios/LICENSE. See LICENSE at the repository root for the
//	full text of the GPL.

import SwiftUI

struct AlbumDetailView: View {
	let ref: ItemRef
	let albumTitle: String

	@Environment(LibraryRepository.self) private var library
	@State private var model: AlbumDetailViewModel?

	var body: some View {
		Group {
			if let model {
				LoadStateBox(state: model.state, onRetry: { model.retry() }) { detail in
					list(detail, model: model)
				}
			} else {
				ProgressView()
			}
		}
		.navigationTitle(albumTitle)
		.navigationBarTitleDisplayMode(.inline)
		.task {
			if model == nil {
				model = AlbumDetailViewModel(library: library, ref: ref)
			}
			model?.appear()
		}
	}

	private func list(_ detail: AlbumDetail, model: AlbumDetailViewModel) -> some View {
		List {
			Section {
				header(detail, model: model)
			}
			.listRowSeparator(.hidden)

			// Disc headings **only when there is more than one disc**. A single
			// heading over every track on a single-disc album says nothing and
			// costs a row.
			if detail.isMultiDisc {
				ForEach(discs(detail), id: \.number) { disc in
					Section {
						ForEach(disc.songs) { TrackRow(song: $0) }
					} header: {
						SectionHeading(text: "Disc \(disc.number)")
					}
				}
			} else {
				Section {
					ForEach(detail.songs) { TrackRow(song: $0) }
				}
			}
		}
		.listStyle(.plain)
		.refreshable { await model.refresh() }
	}

	private func header(_ detail: AlbumDetail, model: AlbumDetailViewModel) -> some View {
		VStack(alignment: .leading, spacing: 12) {
			if model.heroes.count > 1 {
				// The extras `getAlbumImages` counted — booklet scans, a back
				// cover — as a pager rather than a wall of thumbnails.
				TabView {
					ForEach(model.heroes, id: \.cacheKey) { CoverHero(source: $0) }
				}
				.tabViewStyle(.page)
				.aspectRatio(1, contentMode: .fit)
			} else {
				CoverHero(source: model.heroes.first)
			}

			VStack(alignment: .leading, spacing: 2) {
				Text(detail.album.title).font(.title3.weight(.semibold))
				Text(subtitle(detail)).font(.subheadline).foregroundStyle(.secondary)
			}

			if let notes = model.notes {
				NotesSection(text: notes.notes, links: notes.externalLinks)
			}
		}
		.padding(.vertical, 4)
	}

	private func subtitle(_ detail: AlbumDetail) -> String {
		var parts: [String] = []
		if !detail.album.artistName.isEmpty { parts.append(detail.album.artistName) }
		if let year = detail.album.year { parts.append(String(year)) }
		parts.append(detail.songs.count == 1 ? "1 track" : "\(detail.songs.count) tracks")
		if detail.album.duration > 0 { parts.append(formatDuration(detail.album.duration)) }
		return parts.joined(separator: " · ")
	}

	private struct Disc {
		let number: Int
		let songs: [Song]
	}

	private func discs(_ detail: AlbumDetail) -> [Disc] {
		Dictionary(grouping: detail.songs) { $0.discNumber ?? 1 }
			.sorted { $0.key < $1.key }
			.map { Disc(number: $0.key, songs: $0.value) }
	}
}
