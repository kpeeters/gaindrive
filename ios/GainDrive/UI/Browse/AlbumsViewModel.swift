//	GainDrive for iOS
//	Copyright (C) 2026 Kasper Peeters
//
//	This file is part of GainDrive, distributed under the GNU General
//	Public License version 3 or later, with the Application Distribution
//	Exception in ios/LICENSE. See LICENSE at the repository root for the
//	full text of the GPL.

import Foundation

/// One artist's albums, across every server that has them.
///
/// Owned by the screen rather than by `RootView`, because its state should die
/// with the screen: pushing a different artist is a different subject, not a
/// refresh of this one.
@MainActor
@Observable
final class AlbumsViewModel {
	private(set) var state: Load<[AlbumUi]> = .loading
	private(set) var failures: [ServerFailure] = []
	/// Held **separately from the list**, and never awaited before it.
	/// Answering `getArtistInfo2` may send the server out to MusicBrainz and
	/// Wikipedia, so a biography that is slow or missing must cost the
	/// biography and nothing else.
	private(set) var info: ArtistInfo?
	private(set) var portrait: CoverSource?

	@ObservationIgnored private let library: LibraryRepository
	@ObservationIgnored private let selection: ServerSelection
	@ObservationIgnored private let refs: [ItemRef]
	@ObservationIgnored private var loaded = false
	@ObservationIgnored private var task: Task<Void, Never>?

	init(library: LibraryRepository, selection: ServerSelection, refs: [ItemRef]) {
		self.library = library
		self.selection = selection
		self.refs = refs
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
		let badges = selection.badgeNames
		task?.cancel()
		task = Task { [library, refs] in
			let covers = await library.coverUrls()
			let merged = await library.albumsOfArtist(refs)
			guard !Task.isCancelled else { return }
			self.state = merged.map { albums in
				albums.map {
					AlbumUi(
						album: $0,
						cover: covers.source($0.coverArt, size: CoverSize.thumb),
						badges: $0.sources.compactMap { badges[$0] })
				}
			}.load
			self.failures = merged.failures

			// Started only once the list is on screen, and in its own task, so
			// it cannot delay anything.
			guard let primary = refs.first else { return }
			self.portrait = covers.source(primary, size: CoverSize.portrait)
			self.info = await library.artistInfo(primary)
		}
	}

	func dismissFailures() {
		failures = []
	}
}
