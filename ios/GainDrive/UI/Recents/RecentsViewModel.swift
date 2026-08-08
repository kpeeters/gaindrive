//	GainDrive for iOS
//	Copyright (C) 2026 Kasper Peeters
//
//	This file is part of GainDrive, distributed under the GNU General
//	Public License version 3 or later, with the Application Distribution
//	Exception in ios/LICENSE. See LICENSE at the repository root for the
//	full text of the GPL.

import Foundation

/// Recently played, per server.
///
/// Never interleaved by timestamp, however right that would look: each server
/// only knows what was played against *it*, so one ordering across all of them
/// would imply a completeness that does not exist.
@MainActor
@Observable
final class RecentsViewModel {
	private(set) var state: Load<[ServerSection<SongUi>]> = .loading
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
			let merged = await library.recentSongs(scope: scope)
			guard !Task.isCancelled else { return }
			state = merged.map { sections in
				sections.map { section in
					ServerSection(
						server: section.server,
						items: section.items.map {
							SongUi(
								song: $0,
								cover: covers.source($0.coverArt, size: CoverSize.thumb))
						})
				}
			}.load
			failures = merged.failures
		}
	}

	func dismissFailures() {
		failures = []
	}
}
