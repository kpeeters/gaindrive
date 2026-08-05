//	GainDrive for iOS
//	Copyright (C) 2026 Kasper Peeters
//
//	This file is part of GainDrive, distributed under the GNU General
//	Public License version 3 or later, with the Application Distribution
//	Exception in ios/LICENSE. See LICENSE at the repository root for the
//	full text of the GPL.

import Foundation

/// One album: hero artwork, notes, and the track list.
@MainActor
@Observable
final class AlbumDetailViewModel {
	private(set) var state: Load<AlbumDetail> = .loading
	/// As in `AlbumsViewModel`, held apart from the tracks and never awaited
	/// before them — `getAlbumInfo2` has the same MusicBrainz round trip behind
	/// it as the artist biography.
	private(set) var notes: AlbumNotes?
	private(set) var heroes: [CoverSource] = []

	@ObservationIgnored private let library: LibraryRepository
	@ObservationIgnored private let ref: ItemRef
	@ObservationIgnored private var loaded = false
	@ObservationIgnored private var task: Task<Void, Never>?

	init(library: LibraryRepository, ref: ItemRef) {
		self.library = library
		self.ref = ref
	}

	func appear() {
		guard !loaded else { return }
		loaded = true
		start(clearFirst: true)
	}

	func retry() {
		start(clearFirst: true)
	}

	func refresh() async {
		start(clearFirst: false)
		await task?.value
	}

	private func start(clearFirst: Bool) {
		if clearFirst { state = .loading }
		task?.cancel()
		task = Task { [library, ref] in
			let covers = await library.coverUrls()
			let detail: AlbumDetail
			do {
				guard let loaded = try await library.albumDetail(ref) else {
					state = .failed("That album is no longer on the server.")
					return
				}
				guard !Task.isCancelled else { return }
				detail = loaded
				state = .ready(detail)
				heroes = [covers.source(detail.album.coverArt, size: CoverSize.hero)].compactMap { $0 }
			} catch {
				guard !error.isCancellation else { return }
				// A single-ref read throws on purpose: "unreachable" and "no
				// such album" want different words, and the repository
				// swallowing the difference would make them the same screen.
				state = .failed(error.userMessage)
				return
			}

			// Extras, after the tracks are on screen. A failure here costs the
			// carousel and the notes, not the album.
			let extraCount = await library.albumImageCount(ref)
			if extraCount > 1 {
				heroes += (1..<extraCount).compactMap {
					covers.source(detail.album.coverArt, size: CoverSize.hero, index: $0)
				}
			}
			notes = await library.albumNotes(ref)
		}
	}
}
