//	GainDrive for iOS
//	Copyright (C) 2026 Kasper Peeters
//
//	This file is part of GainDrive, distributed under the GNU General
//	Public License version 3 or later, with the Application Distribution
//	Exception in ios/LICENSE. See LICENSE at the repository root for the
//	full text of the GPL.

import Foundation

struct SearchFilters: Equatable, Sendable {
	var artists = true
	var albums = true
	var songs = true

	var noneSelected: Bool { !artists && !albums && !songs }

	/// Zero for a switched-off category, so the server does no work for
	/// something the user has said they do not want.
	var limits: SearchLimits {
		SearchLimits(
			artists: artists ? 20 : 0,
			albums: albums ? 30 : 0,
			songs: songs ? 60 : 0)
	}
}

/// Search has a state the browse screens do not: **nothing typed yet**.
///
/// That is why this exists rather than reusing `Load`. Folding "no query" into
/// "loaded but empty" would show "no results" before the user has asked
/// anything, which is the same class of mistake `Load` itself exists to
/// prevent.
enum SearchPhase {
	case idle
	case searching
	case failed(String)
	case ready(SearchResults)
}

struct SearchResults: Sendable {
	var artists: [ArtistUi] = []
	var albums: [AlbumUi] = []
	var songs: [SongUi] = []
	var failures: [ServerFailure] = []
	/// True while at least one server has not answered. Drives a quiet
	/// indicator rather than a blocking one — the results already on screen are
	/// usable while the slow server is still thinking.
	var outstanding = false

	var isEmpty: Bool { artists.isEmpty && albums.isEmpty && songs.isEmpty }
}

@MainActor
@Observable
final class SearchViewModel {
	var query = "" {
		didSet { queryChanged() }
	}
	var filters = SearchFilters() {
		// **Only the query is debounced.** Toggling a filter is a deliberate
		// act and should re-run at once; waiting a third of a second after a
		// tap feels broken in a way that waiting after a keystroke does not.
		didSet { if filters != oldValue { restart() } }
	}

	private(set) var phase: SearchPhase = .idle

	@ObservationIgnored private let library: LibraryRepository
	@ObservationIgnored private let selection: ServerSelection
	@ObservationIgnored private var task: Task<Void, Never>?

	/// Below this a query matches most of the library, and every server pays
	/// for it on the second keystroke of every word.
	private static let minimumQuery = 2
	private static let debounce = Duration.milliseconds(300)

	init(library: LibraryRepository, selection: ServerSelection) {
		self.library = library
		self.selection = selection
	}

	/// Called when the scope changes. An unchanged query against a different
	/// set of servers is a different question — but with nothing typed there is
	/// no question to re-ask.
	func scopeChanged() {
		if case .idle = phase { return }
		restart()
	}

	/// Re-runs the current query. Needed as its own entry point because the
	/// obvious alternative — reassigning `filters` — is guarded against an
	/// unchanged value and would silently do nothing.
	func retry() {
		restart()
	}

	func clear() {
		query = ""
	}

	private func queryChanged() {
		let trimmed = query.trimmingCharacters(in: .whitespacesAndNewlines)
		guard trimmed.count >= Self.minimumQuery else {
			task?.cancel()
			phase = .idle
			return
		}
		restart()
	}

	private func restart() {
		let trimmed = query.trimmingCharacters(in: .whitespacesAndNewlines)
		guard trimmed.count >= Self.minimumQuery, !filters.noneSelected else {
			task?.cancel()
			phase = .idle
			return
		}

		let scope = selection.scope
		let limits = filters.limits
		let badges = selection.badgeNames
		task?.cancel()
		task = Task { [library] in
			// The debounce lives here rather than in the view so that a
			// cancelled task cancels the wait too — a `.task(id:)` would
			// restart the timer but leave the previous request running.
			try? await Task.sleep(for: Self.debounce)
			guard !Task.isCancelled else { return }

			phase = .searching
			let covers = await library.coverUrls()
			var seen = false

			for await merged in library.searchProgressively(
				scope: scope, query: trimmed, limits: limits)
			{
				guard !Task.isCancelled else { return }
				seen = true
				phase = .ready(
					Self.present(merged, covers: covers, badges: badges, outstanding: true))
			}
			guard !Task.isCancelled else { return }

			// The stream finishing is what turns the indicator off; the last
			// emission cannot know it was the last.
			if case .ready(var results) = phase {
				results.outstanding = false
				phase = .ready(results)
			} else if !seen {
				phase = .ready(SearchResults())
			}
		}
	}

	private static func present(
		_ merged: MergedResult<LibrarySelection>,
		covers: CoverUrls,
		badges: [ServerId: String],
		outstanding: Bool
	) -> SearchResults {
		SearchResults(
			artists: merged.items.artists.map {
				ArtistUi(artist: $0, badges: $0.sources.compactMap { badges[$0] })
			},
			albums: merged.items.albums.map {
				AlbumUi(
					album: $0,
					cover: covers.source($0.coverArt, size: CoverSize.thumb),
					badges: $0.sources.compactMap { badges[$0] })
			},
			songs: merged.items.songs.map {
				SongUi(
					song: $0,
					cover: covers.source($0.coverArt, size: CoverSize.thumb),
					badge: badges[$0.ref.server])
			},
			failures: merged.failures,
			outstanding: outstanding)
	}
}
