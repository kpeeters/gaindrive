//	GainDrive for iOS
//	Copyright (C) 2026 Kasper Peeters
//
//	This file is part of GainDrive, distributed under the GNU General
//	Public License version 3 or later, with the Application Distribution
//	Exception in ios/LICENSE. See LICENSE at the repository root for the
//	full text of the GPL.

import Foundation

/// What the Recents screen shows: albums the server first saw recently, then
/// what was played.
struct RecentsContent: Equatable, Sendable {
	var added: [ServerSection<AlbumUi>] = []
	var played: [ServerSection<SongUi>] = []

	var isEmpty: Bool { added.isEmpty && played.isEmpty }
}

// The same "empty and partial is failed" rule as the other listings.
extension MergedResult where Value == RecentsContent {
	var load: Load<Value> {
		if items.isEmpty, let first = failures.first {
			return .failed(first.message)
		}
		return .ready(items)
	}
}

/// Recently added and recently played, per server.
///
/// Never interleaved by timestamp, however right that would look: each server
/// only knows what was played against *it*, so one ordering across all of them
/// would imply a completeness that does not exist.
@MainActor
@Observable
final class RecentsViewModel {
	private(set) var state: Load<RecentsContent> = .loading
	private(set) var failures: [ServerFailure] = []

	@ObservationIgnored private let library: LibraryRepository
	@ObservationIgnored private let selection: ServerSelection
	@ObservationIgnored private var loadedFor: BrowseScope?
	@ObservationIgnored private var task: Task<Void, Never>?

	init(library: LibraryRepository, selection: ServerSelection) {
		self.library = library
		self.selection = selection
	}

	/// Unlike the other tabs, coming back here *is* a reason to re-read: the
	/// whole point of the screen is what happened since the user last looked,
	/// and it changes while they are elsewhere in the app.
	func appear() {
		start(clearFirst: loadedFor != selection.scope)
	}

	func retry() {
		start(clearFirst: true)
	}

	func refresh() async {
		start(clearFirst: false)
		await task?.value
	}

	private func start(clearFirst: Bool) {
		let scope = selection.scope
		loadedFor = scope
		if clearFirst { state = .loading }
		task?.cancel()
		task = Task { [library] in
			let covers = await library.coverUrls()
			async let addedResult = library.recentlyAdded(scope: scope, size: 12)
			async let playedResult = library.recentSongs(scope: scope)
			let (added, played) = await (addedResult, playedResult)
			guard !Task.isCancelled else { return }
			let content = RecentsContent(
				added: added.items.map { section in
					ServerSection(
						server: section.server,
						items: section.items.map {
							AlbumUi(
								album: $0,
								cover: covers.source($0.coverArt, size: CoverSize.thumb))
						})
				},
				played: played.items.map { section in
					ServerSection(
						server: section.server,
						items: section.items.map {
							SongUi(
								song: $0,
								cover: covers.source($0.coverArt, size: CoverSize.thumb))
						})
				})
			// A server that is down fails both queries; say so once.
			var seen = Set<ServerId>()
			let merged = MergedResult(
				items: content,
				failures: (added.failures + played.failures).filter { seen.insert($0.server).inserted })
			state = merged.load
			failures = merged.failures
		}
	}

	func dismissFailures() {
		failures = []
	}
}
