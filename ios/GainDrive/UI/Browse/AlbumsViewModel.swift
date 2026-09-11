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
	private(set) var sort: AlbumSort

	@ObservationIgnored private let library: LibraryRepository
	@ObservationIgnored private let selection: ServerSelection
	@ObservationIgnored private let settings: SettingsStore
	@ObservationIgnored private let refs: [ItemRef]
	/// A section of a **categories** root: Film and Series are not performers,
	/// so there is no portrait to fetch and no biography to wait for.
	@ObservationIgnored private let fromCategories: Bool
	/// Which section's preference the sort belongs to, derived from the route
	/// flags the listing was drilled in from — this used to read a stored
	/// "current chip", which could disagree with the listing on screen.
	@ObservationIgnored private let section: LibrarySection
	@ObservationIgnored private var loaded = false
	@ObservationIgnored private var task: Task<Void, Never>?

	init(
		library: LibraryRepository, selection: ServerSelection, settings: SettingsStore,
		refs: [ItemRef], fromCategories: Bool, fromUploads: Bool = false
	) {
		self.library = library
		self.selection = selection
		self.settings = settings
		self.refs = refs
		self.fromCategories = fromCategories
		let section: LibrarySection =
			fromUploads ? .uploads : fromCategories ? .categories : .artists
		self.section = section
		self.sort = settings.albumSort(for: section)
	}

	/// **Re-sorted in the client, never asked of the server.** The whole list
	/// is already in hand, so the order changes with no request and no spinner.
	///
	/// It is also what fixes a merged artist's order, which was never in year
	/// order at all: `Merge.albums` keeps arrival order, so the union was one
	/// server's list concatenated with the next's.
	func setSort(_ next: AlbumSort) {
		guard next != sort else { return }
		sort = next
		settings.setAlbumSort(next, for: section)
		guard case .ready(let items) = state else { return }
		state = .ready(items.sorted { sort.precedes($0.album, $1.album) })
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
				.sorted { self.sort.precedes($0.album, $1.album) }
			}.load
			self.failures = merged.failures

			// Started only once the list is on screen, and in its own task, so
			// it cannot delay anything.
			//
			// **Not at all for a categories section.** The server refuses the
			// lookup for one (`is_category_folder()`), so the avatar would sit
			// as a placeholder for ever while the fetch retried a 404 — and
			// `AlbumsView` draws no header when both of these stay nil.
			guard !self.fromCategories, let primary = refs.first else { return }
			self.portrait = covers.source(primary, size: CoverSize.portrait)
			self.info = await library.artistInfo(primary)
		}
	}

	func dismissFailures() {
		failures = []
	}
}
