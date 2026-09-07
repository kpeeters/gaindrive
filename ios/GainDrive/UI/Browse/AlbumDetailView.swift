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
	/// A track to start once the listing has loaded — see `Route.album`.
	var autoPlay: ItemRef?

	@Environment(\.library) private var library
	@Environment(PlayerConnection.self) private var player
	@State private var model: AlbumDetailViewModel?
	/// Once per screen, not once per load: a pull-to-refresh must not restart
	/// the track the user has since navigated away from inside.
	@State private var autoPlayed = false

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
			if model == nil, let library {
				model = AlbumDetailViewModel(library: library, ref: ref)
			}
			model?.appear()
		}
	}

	private func list(_ detail: AlbumDetail, model: AlbumDetailViewModel) -> some View {
		// An album ripped with no tags, or with every track tagged 1, carries
		// no usable numbering — number the rows by position rather than leave
		// the column blank. The server derives a number from a numbered
		// filename, so this is the remainder: files named without one. Matches
		// web/app.js and the Android client, album-wide index included, so the
		// three read the same.
		let useSeq = detail.songs.allSatisfy { ($0.track ?? 0) <= 1 }

		return List {
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
						ForEach(disc.songs) { song in
							trackRow(song, in: detail, useSeq: useSeq)
						}
					} header: {
						SectionHeading(text: "Disc \(disc.number)")
					}
				}
			} else {
				Section {
					ForEach(detail.songs) { song in
						trackRow(song, in: detail, useSeq: useSeq)
					}
				}
			}
		}
		.listStyle(.plain)
		.refreshable { await model.refresh() }
		.task(id: detail) { startAutoPlay(detail) }
	}

	/// A ref naming a track this album no longer holds does nothing, silently.
	/// The listing is the newer fact, and a hit that has gone is not an error
	/// worth a dialog over.
	private func startAutoPlay(_ detail: AlbumDetail) {
		guard let autoPlay, !autoPlayed else { return }
		autoPlayed = true
		guard let index = detail.songs.firstIndex(where: { $0.ref == autoPlay }) else { return }
		player.play(detail.songs, startIndex: index)
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

			HStack(alignment: .firstTextBaseline) {
				VStack(alignment: .leading, spacing: 2) {
					Text(detail.album.title).font(.title3.weight(.semibold))
					Text(subtitle(detail)).font(.subheadline).foregroundStyle(.secondary)
				}
				Spacer(minLength: 12)
				StarButton(
					ref: detail.album.ref, kind: .album, starredAt: detail.album.starredAt)
			}

			if let notes = model.notes {
				NotesSection(text: notes.notes, links: notes.externalLinks)
			}
		}
		.padding(.vertical, 4)
	}

	/// **The index is into the flat `detail.songs`, never into the disc slice.**
	/// The multi-disc branch renders a grouped slice, and taking the slice index
	/// would play the wrong track on every disc after the first — silently, and
	/// invisibly to anyone testing with a single-disc album.
	private func trackRow(_ song: Song, in detail: AlbumDetail, useSeq: Bool)
		-> some View
	{
		let index = detail.songs.firstIndex(of: song)
		return Button {
			guard let index else { return }
			player.play(detail.songs, startIndex: index)
		} label: {
			TrackRow(
				song: song,
				state: player.trackState(of: song.ref),
				number: useSeq ? index.map { $0 + 1 } : song.track)
		}
		.buttonStyle(.plain)
		.trackActions(for: song)
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
