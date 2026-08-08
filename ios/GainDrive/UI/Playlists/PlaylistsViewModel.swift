//	GainDrive for iOS
//	Copyright (C) 2026 Kasper Peeters
//
//	This file is part of GainDrive, distributed under the GNU General
//	Public License version 3 or later, with the Application Distribution
//	Exception in ios/LICENSE. See LICENSE at the repository root for the
//	full text of the GPL.

import Foundation

/// Playlists, in per-server sections.
///
/// Sections rather than one merged list, because playlists are per-account
/// server-side state: a global ordering across servers would look right and be
/// wrong, since each server only knows about its own.
@MainActor
@Observable
final class PlaylistsViewModel {
	private(set) var state: Load<[ServerSection<Playlist>]> = .loading
	private(set) var failures: [ServerFailure] = []
	private(set) var error: String?

	@ObservationIgnored private let library: LibraryRepository
	@ObservationIgnored private let selection: ServerSelection
	@ObservationIgnored private let events: LibraryEvents
	@ObservationIgnored private var loadedScope: BrowseScope?
	@ObservationIgnored private var loadedRevision = 0
	@ObservationIgnored private var task: Task<Void, Never>?

	init(library: LibraryRepository, selection: ServerSelection, events: LibraryEvents) {
		self.library = library
		self.selection = selection
		self.events = events
	}

	/// A scope change blanks the list; a revision bump does not.
	///
	/// The distinction matters: blanking on every edit would flash the whole
	/// screen each time a track was added to a playlist from somewhere else.
	func appear() {
		guard let loadedScope else {
			start(clearFirst: true)
			return
		}
		let scopeChanged = loadedScope != selection.scope
		guard scopeChanged || loadedRevision != events.playlistRevision else { return }
		start(clearFirst: scopeChanged)
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
		loadedScope = scope
		loadedRevision = events.playlistRevision
		if clearFirst { state = .loading }
		task?.cancel()
		task = Task { [library] in
			let merged = await library.playlists(scope: scope)
			guard !Task.isCancelled else { return }
			state = merged.load
			failures = merged.failures
		}
	}

	/// **No optimistic removal.** The reload that the revision bump triggers is
	/// what takes the row away, so the list can never claim a deletion the
	/// server refused — which is a promise the UI is in no position to make,
	/// given it cannot know the playlist was someone else's.
	func delete(_ playlist: Playlist) async {
		do {
			try await library.deletePlaylist(playlist.ref)
			appear()
		} catch {
			self.error = error.userMessage
		}
	}

	func clearError() {
		error = nil
	}

	func dismissFailures() {
		failures = []
	}
}
