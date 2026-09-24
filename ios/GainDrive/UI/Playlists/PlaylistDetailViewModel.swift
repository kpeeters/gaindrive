//	GainDrive for iOS
//	Copyright (C) 2026 Kasper Peeters
//
//	This file is part of GainDrive, distributed under the GNU General
//	Public License version 3 or later, with the Application Distribution
//	Exception in ios/LICENSE. See LICENSE at the repository root for the
//	full text of the GPL.

import Foundation

/// One playlist's tracks, and removing them.
@MainActor
@Observable
final class PlaylistDetailViewModel {
	private(set) var state: Load<[SongUi]> = .loading
	private(set) var error: String?
	/// A removal is in flight. **The guard, not a spinner.** See `remove`.
	private(set) var isRemoving = false

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
			do {
				guard let playlist = try await library.playlist(ref) else {
					state = .failed("That playlist is no longer on the server.")
					return
				}
				guard !Task.isCancelled else { return }
				state = .ready(
					playlist.songs.map {
						SongUi(song: $0, cover: covers.source($0.coverArt, size: CoverSize.thumb))
					})
			} catch {
				guard !error.isCancellation else { return }
				state = .failed(error.userMessage)
			}
		}
	}

	/// Removes the track at `index`.
	///
	/// **Serialised, and that is the whole point of `isRemoving`.** The endpoint
	/// removes by *position*, and positions shift the moment one is gone - so a
	/// second removal issued before the first has landed would carry an index
	/// computed against the pre-removal list and delete the wrong track. The
	/// guard is correctness, not a progress indicator.
	///
	/// This one *does* drop the row locally before the server confirms, unlike
	/// the playlists list, because the index the next removal uses has to come
	/// from a list that already reflects this one.
	func remove(at index: Int) async {
		guard !isRemoving, var songs = state.value, songs.indices.contains(index) else { return }
		isRemoving = true
		defer { isRemoving = false }

		songs.remove(at: index)
		state = .ready(songs)

		do {
			try await library.removeFromPlaylist(ref, at: index)
		} catch {
			self.error = error.userMessage
			// Put the list back the way the server still has it, rather than
			// leaving the user looking at a removal that did not happen.
			await refresh()
		}
	}

	func clearError() {
		error = nil
	}
}
