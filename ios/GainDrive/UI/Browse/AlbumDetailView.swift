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
	/// Where in that track to start, in seconds. Non-zero for a chapter hit,
	/// which names a marker inside a recording rather than the recording.
	var autoPlayAt: Double = 0

	@Environment(\.library) private var library
	@Environment(PlayerConnection.self) private var player
	@Environment(PinRepository.self) private var pins
	@Environment(SettingsStore.self) private var settings
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
		.toolbar {
			ToolbarItem(placement: .topBarTrailing) {
				DownloadControl(pin: Pin(ref: ref, kind: .album, name: albumTitle))
			}
		}
		.task {
			if model == nil, let library {
				model = AlbumDetailViewModel(library: library, ref: ref)
			}
			model?.appear()
		}
	}

	private func list(_ detail: AlbumDetail, model: AlbumDetailViewModel) -> some View {
		// One entry per *row*, not per song: a chaptered recording is replaced
		// by its markers, so the two are no longer the same count. The
		// numbering and heading rules live in `albumListRows`, where they can
		// be tested without a screen or a player.
		let rows = albumListRows(songs: detail.songs, chapters: model.chapters)
		// Which marker is playing, computed once for the whole list rather than
		// per row. It cannot come from `trackState(of:)`, which answers about a
		// song: every marker of a playing concert would be current at once.
		//
		// **The empty branch is what keeps an album with no chapters — nearly
		// every album — costing exactly what it did before.** `position` is
		// never read there, so the twice-a-second tick does not invalidate a
		// listing that has nothing to highlight. That is the rule
		// `PlayerConnection` states about `trackState(of:)`, applied to the one
		// place that has to break it.
		let playingMarker: PlayingMarker? =
			model.chapters.isEmpty ? nil : currentMarker(model.chapters)

		return List {
			Section {
				header(detail, model: model)
			}
			.listRowSeparator(.hidden)

			// Disc headings **only when there is more than one disc**. A single
			// heading over every track on a single-disc album says nothing and
			// costs a row.
			if detail.isMultiDisc {
				ForEach(discs(rows), id: \.number) { disc in
					Section {
						ForEach(disc.rows) { row in
							listRow(row, in: detail, playing: playingMarker)
						}
					} header: {
						SectionHeading(text: "Disc \(disc.number)")
					}
				}
			} else {
				Section {
					ForEach(rows) { row in
						listRow(row, in: detail, playing: playingMarker)
					}
				}
			}
		}
		.listStyle(.plain)
		.refreshable { await model.refresh() }
		.task(id: detail) { startAutoPlay(detail) }
	}

	/// The recording being played and which of its markers, as a pair, so one
	/// comparison decides every row.
	private struct PlayingMarker: Equatable {
		let song: ItemRef
		let index: Int
	}

	private func currentMarker(_ chapters: [ItemRef: [Chapter]]) -> PlayingMarker? {
		guard let ref = player.current?.ref, let markers = chapters[ref],
			let at = markers.currentIndex(at: player.position)
		else {
			return nil
		}
		return PlayingMarker(song: ref, index: markers[at].index)
	}

	@ViewBuilder
	private func listRow(_ row: AlbumListRow, in detail: AlbumDetail, playing: PlayingMarker?)
		-> some View
	{
		// A heading naming the recording, drawn inside the row it introduces so
		// that the list's identity stays one entry per row. `albumListRows`
		// decides whether there is one at all.
		if let heading = row.recordingHeading {
			SectionHeading(text: heading)
				.padding(.top, 8)
				.listRowSeparator(.hidden)
		}
		switch row.kind {
		case .track(let number):
			trackRow(row.song, number: number, in: detail, queueIndex: row.queueIndex)
		case .marker(let chapter):
			markerRow(row, chapter: chapter, in: detail, playing: playing)
		}
	}

	private func markerRow(
		_ row: AlbumListRow, chapter: Chapter, in detail: AlbumDetail, playing: PlayingMarker?
	) -> some View {
		let unplayable = settings.offlineMode && !pins.state(for: row.song.ref).isHere
		return Button {
			// The album queue, starting at the recording, positioned at the
			// marker — which is what tapping a song of a concert should mean.
			player.play(detail.songs, startIndex: row.queueIndex, startPosition: chapter.start)
		} label: {
			ChapterRow(
				chapter: chapter,
				playing: playing == PlayingMarker(song: row.song.ref, index: chapter.index))
		}
		.buttonStyle(.plain)
		.disabled(unplayable)
		.opacity(unplayable ? 0.4 : 1)
		// The recording's own row is gone, so this is the only way left to
		// reach its actions — starring the concert, queueing it, downloading it.
		.trackActions(for: row.song)
	}

	/// A ref naming a track this album no longer holds does nothing, silently.
	/// The listing is the newer fact, and a hit that has gone is not an error
	/// worth a dialog over.
	private func startAutoPlay(_ detail: AlbumDetail) {
		guard let autoPlay, !autoPlayed else { return }
		autoPlayed = true
		guard let index = detail.songs.firstIndex(where: { $0.ref == autoPlay }) else { return }
		player.play(detail.songs, startIndex: index, startPosition: autoPlayAt)
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

	/// **`queueIndex` is into the flat `detail.songs`, never into the disc
	/// slice.** The multi-disc branch renders a grouped slice, and taking the
	/// slice index would play the wrong track on every disc after the first —
	/// silently, and invisibly to anyone testing with a single-disc album.
	/// `albumListRows` computes it once, before any grouping, which is what
	/// makes it right for a marker too.
	private func trackRow(_ song: Song, number: Int?, in detail: AlbumDetail, queueIndex: Int)
		-> some View
	{
		// **Dimmed and inert, not hidden.** Inside an album, knowing what is
		// missing is the useful part — and dropping rows would renumber the
		// record. `android/CACHING.md` draws the same line between a listing,
		// which shrinks, and a track list, which does not.
		let unplayable = settings.offlineMode && !pins.state(for: song.ref).isHere
		return Button {
			player.play(detail.songs, startIndex: queueIndex)
		} label: {
			TrackRow(song: song, state: player.trackState(of: song.ref), number: number)
		}
		.buttonStyle(.plain)
		.disabled(unplayable)
		.opacity(unplayable ? 0.4 : 1)
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
		let rows: [AlbumListRow]
	}

	/// Grouped from the *flattened* rows, so a chaptered recording's markers
	/// land under the disc heading its recording belongs to. `Dictionary`'s
	/// grouping keeps each group in the order it met them, which is album
	/// order.
	private func discs(_ rows: [AlbumListRow]) -> [Disc] {
		Dictionary(grouping: rows, by: \.disc)
			.sorted { $0.key < $1.key }
			.map { Disc(number: $0.key, rows: $0.value) }
	}
}
