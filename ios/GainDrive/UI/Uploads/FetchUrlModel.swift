//	GainDrive for iOS
//	Copyright (C) 2026 Kasper Peeters
//
//	This file is part of GainDrive, distributed under the GNU General
//	Public License version 3 or later, with the Application Distribution
//	Exception in ios/LICENSE. See LICENSE at the repository root for the
//	full text of the GPL.

import Foundation

/// A name already in the library, for the advisory check.
struct NameSuggestion: Sendable {
	let name: String
	let refs: [ItemRef]
	/// "Artists", "Categories" or "your uploads".
	let place: String
	/// In the uploads area, not yet moved into the library.
	let staging: Bool
}

/// The fetch form. A port of Android's `FetchUrlViewModel`, minus the share
/// intake: iOS takes the URL pasted, there being no Share Extension.
@MainActor
@Observable
final class FetchUrlModel {
	var url = ""
	var artist = "" {
		didSet { recheck() }
	}
	var album = "" {
		didSet { recheck() }
	}
	var audio = true {
		didSet { settings.fetchAudio = audio }
	}
	var server: ServerId? {
		didSet {
			settings.fetchServer = server?.description
			alignMode()
		}
	}
	private(set) var targets: [FetchTarget]?
	private(set) var submitting = false
	var error: String?
	/// "You already have …" - advisory, and **never blocks**: fetching a
	/// second copy on purpose is a real thing to want.
	private(set) var existing: String?

	@ObservationIgnored private let fetches: Fetches
	@ObservationIgnored private let library: LibraryRepository
	@ObservationIgnored private let settings: SettingsStore
	@ObservationIgnored private var suggestions: [NameSuggestion] = []
	@ObservationIgnored private var checker: Task<Void, Never>?

	init(fetches: Fetches, library: LibraryRepository, settings: SettingsStore) {
		self.fetches = fetches
		self.library = library
		self.settings = settings
		// Observers do not run for assignments in the type's own initialiser,
		// so these do not write the settings straight back.
		artist = settings.fetchArtist
		album = settings.fetchAlbum
		audio = settings.fetchAudio
	}

	var target: FetchTarget? { targets?.first { $0.id == server } }

	/// The chosen server's jobs, newest first.
	var jobs: [FetchJob] { fetches.jobs.filter { $0.server == server } }

	var contactLost: Bool { server.map { fetches.contactLost.contains($0) } ?? false }

	/// The same URL already fetched or fetching on this server. A live one
	/// refuses a second submit; a finished one is only mentioned.
	var duplicate: FetchJob? {
		let wanted = url.trimmingCharacters(in: .whitespacesAndNewlines)
		guard !wanted.isEmpty else { return nil }
		let mine = jobs.filter { $0.url == wanted }
		return mine.first { $0.state.isLive } ?? mine.first
	}

	var showsModes: Bool { target.map { $0.canAudio && $0.canVideo } ?? false }

	var canSubmit: Bool {
		!url.trimmingCharacters(in: .whitespaces).isEmpty && server != nil && !submitting
			&& duplicate?.state.isLive != true
	}

	func appear() async {
		fetches.nudge()
		let found = await fetches.targets()
		targets = found
		// A server with something running first, then the one used last, then
		// the first there is.
		let busy = Set(fetches.live.map(\.server))
		let remembered = settings.fetchServer
		server =
			found.first { busy.contains($0.id) }?.id
			?? found.first { $0.id.description == remembered }?.id
			?? found.first?.id
		await loadSuggestions()
		recheck()
	}

	/// Keeps the mode on something the chosen server can produce.
	private func alignMode() {
		guard let target else { return }
		if audio, !target.canAudio { audio = false }
		if !audio, !target.canVideo { audio = target.canAudio }
	}

	func clearNames() {
		artist = ""
		album = ""
		settings.fetchArtist = ""
		settings.fetchAlbum = ""
	}

	func submit() {
		guard canSubmit, let server else { return }
		submitting = true
		error = nil
		settings.fetchArtist = artist.trimmingCharacters(in: .whitespaces)
		settings.fetchAlbum = album.trimmingCharacters(in: .whitespaces)
		Task {
			do {
				try await fetches.submit(
					on: server, url: url, audio: audio, artist: artist, album: album)
				url = ""
			} catch {
				self.error = "Could not start: \(error.userMessage)"
			}
			submitting = false
		}
	}

	func cancel(_ job: FetchJob) {
		Task { await fetches.cancel(job) }
	}

	// MARK: - The advisory check

	private func loadSuggestions() async {
		var seen: [String: NameSuggestion] = [:]
		var order: [String] = []
		func note(_ artists: [Artist], place: String, staging: Bool) {
			for artist in artists {
				let key = artist.name.lowercased()
				guard seen[key] == nil else { continue }
				seen[key] = NameSuggestion(
					name: artist.name, refs: artist.refs, place: place, staging: staging)
				order.append(key)
			}
		}
		let listing = await library.libraryListing(scope: .allServers).items
		note(listing.categories, place: "Categories", staging: false)
		note(listing.artists.flatMap(\.artists), place: "Artists", staging: false)
		let uploads = await library.uploadIndexes(scope: .allServers).items
		note(uploads.flatMap(\.artists), place: "your uploads", staging: true)
		suggestions = order.compactMap { seen[$0] }
	}

	/// Debounced, so typing does not fire a search per keystroke.
	private func recheck() {
		checker?.cancel()
		checker = Task {
			try? await Task.sleep(for: .milliseconds(400))
			guard !Task.isCancelled else { return }
			let found = await describeExisting()
			guard !Task.isCancelled else { return }
			existing = found
		}
	}

	private func describeExisting() async -> String? {
		let artist = artist.trimmingCharacters(in: .whitespaces)
		let album = album.trimmingCharacters(in: .whitespaces)
		if let hit = suggestions.first(where: {
			$0.name.caseInsensitiveCompare(artist) == .orderedSame
		}) {
			let place = hit.staging ? "in your uploads, not yet moved" : "in \(hit.place)"
			if !album.isEmpty {
				let albums = await library.albumsOfArtist(hit.refs).items
				if let match = albums.first(where: {
					$0.title.caseInsensitiveCompare(album) == .orderedSame
				}) {
					return "You already have “\(match.title)” under \(hit.name), \(place)."
				}
			}
			return "“\(hit.name)” is already \(place)."
		}
		guard album.count >= 3 else { return nil }
		var last: LibrarySelection?
		for await result in library.searchProgressively(
			scope: .allServers, query: album,
			limits: SearchLimits(artists: 0, albums: 3, songs: 0, chapters: 0))
		{
			last = result.items
		}
		guard let first = last?.albums.first else { return nil }
		var text = "Possibly already there: “\(first.title)”"
		if !first.artistName.isEmpty { text += " by \(first.artistName)" }
		if let count = last?.albums.count, count > 1 { text += " and \(count - 1) more" }
		return text + "."
	}
}
