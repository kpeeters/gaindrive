//	GainDrive for iOS
//	Copyright (C) 2026 Kasper Peeters
//
//	This file is part of GainDrive, distributed under the GNU General
//	Public License version 3 or later, with the Application Distribution
//	Exception in ios/LICENSE. See LICENSE at the repository root for the
//	full text of the GPL.

import Foundation

/// Every read and write the browse screens make, mirroring
/// `data/LibraryRepository.kt`.
///
/// One rule makes the whole API readable at a glance:
///
///   **Fan-out reads never throw. Single-ref reads throw. Writes throw.**
///
/// The first turns "no branch throws out of the task group" from a discipline
/// into a type-level guarantee. The second is required rather than stylistic:
/// "the server is unreachable" and "there is no such album" want different
/// screens, and swallowing would turn the first into the second.
///
/// **This type must stay stateless.** It is `Sendable` and deliberately not
/// isolated to any actor, which is what lets the fan-out run entirely off the
/// main thread; the moment someone adds a `private var cache`, it stops being
/// `Sendable` and the fan-out closures stop compiling — with an error that
/// reads as being about the closure. Anything that genuinely needs to remember
/// something gets its own isolation domain.
final class LibraryRepository: Sendable {
	private let registry: ServerRegistry
	private let settings: SettingsStore
	private let events: LibraryEvents
	private let accounts: Accounts
	private let roots: MusicRoots

	init(
		registry: ServerRegistry, settings: SettingsStore, events: LibraryEvents,
		accounts: Accounts, roots: MusicRoots
	) {
		self.registry = registry
		self.settings = settings
		self.events = events
		self.accounts = accounts
		self.roots = roots
	}

	// MARK: - Fan-out reads

	/// Which slices this scope offers, as one chip row.
	///
	/// **Empty when nothing answered**, which is not the same as "this library
	/// has only Artists" and must not be flattened into it here: the caller
	/// keeps the row it already has, because losing the chips to one server's
	/// timeout would be worse than showing a stale set. The floor lives in the
	/// view model, which is the only place that knows what is currently on
	/// screen.
	func availableModes(scope: BrowseScope) async -> [LibraryMode] {
		let clients = await registry.clientsSnapshot()
		let gathered = await gather(over: clients.servers(in: scope), clients: clients) {
			[accounts, roots] client, config in
			let facts = await accounts.facts(for: config.id, using: clients)
			let known = try await Self.rootsOf(client, config.id, cache: roots)
			return LibraryRoots.chips(roots: known, canUpload: facts.canUpload)
		}
		return LibraryRoots.mergeChips(perServer: gathered.answers.map(\.1))
	}

	func artistIndexes(scope: BrowseScope, mode: LibraryMode = .artists) async -> MergedResult<
		[ArtistIndex]
	> {
		let clients = await registry.clientsSnapshot()
		let gathered = await gather(over: clients.servers(in: scope), clients: clients) {
			[accounts, roots] client, config in
			// **A server that cannot answer for this chip is not asked at
			// all.** Not asking is the whole point: one predating library roots
			// ignores an unknown `contentType` and answers with its entire
			// library, so the request itself is what would put the same artists
			// under every chip. Filtering the reply would be too late — nothing
			// in it says which rows to discard.
			//
			// Both facts come from the one cached `getUser` per server, so the
			// second costs no request.
			let facts = await accounts.facts(for: config.id, using: clients)
			let known = try await Self.rootsOf(client, config.id, cache: roots)
			guard
				let request = LibraryRoots.request(
					roots: known, mode: mode,
					canUpload: facts.canUpload, isAdmin: facts.isAdmin)
			else { return [] }

			return try await client.artists(
				personal: request.personal.parameter,
				contentType: request.contentType,
				musicFolderId: request.musicFolderId
			).map { LibraryMapper.index($0, server: config.id) }
		}
		let perServer = gathered.answers.map(\.1)
		return MergedResult(
			// **Not merged in the uploads slice.** Everywhere else, two servers
			// holding an artist of the same name are holding the same artist.
			// In uploads they are two people's folders, and the owner buckets
			// exist to keep them apart — merging by name would file one
			// person's upload under another's heading.
			items: mode == .uploads
				? Merge.concatenatedIndexes(perServer: perServer)
				: Merge.artistIndexes(perServer: perServer),
			failures: gathered.failures)
	}

	/// One server's roots, fetched once per session.
	///
	/// A failure propagates rather than resolving to "no roots": that reaches
	/// the caller's `catch` and is reported as a server that did not answer,
	/// which is true — whereas an empty list would silently mean "this server
	/// offers only Artists" and would be indistinguishable from a real answer.
	private static func rootsOf(
		_ client: SubsonicClient, _ server: ServerId, cache: MusicRoots
	) async throws -> [MusicRoot] {
		if let cached = await cache.cached(server) { return cached }
		let fetched = try await client.musicFolders().compactMap(LibraryMapper.musicRoot)
		await cache.store(fetched, for: server)
		return fetched
	}

	/// Takes a *list* of refs because a merged artist row stands for the same
	/// artist on several servers, each with its own id — so the albums screen
	/// has to ask all of them, not just the one whose row was tapped.
	func albumsOfArtist(_ refs: [ItemRef]) async -> MergedResult<[Album]> {
		let clients = await registry.clientsSnapshot()
		// Read before the fan-out, so the transform below stays synchronous.
		let collapse = await settings.mergeDuplicateAlbums

		let gathered = await gather(over: refs, clients: clients) { client, ref in
			let artist = try await client.artist(id: ref.id)
			return (artist?.album ?? []).compactMap {
				LibraryMapper.album($0, server: ref.server)
			}
		}
		let albums = gathered.answers.flatMap(\.1)
		return MergedResult(
			items: collapse ? Merge.albums(albums) : albums,
			failures: gathered.failures)
	}

	func playlists(scope: BrowseScope) async -> MergedResult<[ServerSection<Playlist>]> {
		let clients = await registry.clientsSnapshot()
		let gathered = await gather(over: clients.servers(in: scope), clients: clients) {
			client, config in
			try await client.playlists().compactMap {
				LibraryMapper.playlist($0, server: config.id)
			}
		}
		return MergedResult(items: sections(gathered), failures: gathered.failures)
	}

	func recentSongs(scope: BrowseScope, size: Int = 50) async -> MergedResult<[ServerSection<Song>]> {
		let clients = await registry.clientsSnapshot()
		let gathered = await gather(over: clients.servers(in: scope), clients: clients) {
			client, config in
			// A server that does not implement this extension contributes an
			// empty section rather than a failure: the feature being absent is
			// not the server being down.
			do {
				return try await client.recentSongs(size: size).compactMap {
					LibraryMapper.song($0, server: config.id)
				}
			} catch is SubsonicError {
				// Only a *protocol* refusal is shrugged off. A transport
				// failure or a cancellation is not this endpoint being
				// missing, so it propagates and is reported as normal.
				return []
			}
		}
		return MergedResult(items: sections(gathered), failures: gathered.failures)
	}

	/// One emission per server as it answers, each carrying **everything
	/// received so far** and rebuilt in registry order.
	///
	/// Cumulative rather than incremental, and ordered rather than
	/// chronological, for the same reason: a slow server's rows have to appear
	/// *in place* rather than being appended, or the list shuffles under the
	/// user's finger the moment the second server lands.
	///
	/// Unlike Android, which guards the same accumulation with a `Mutex`, the
	/// results are folded in by a single serial loop — structured concurrency
	/// is the lock, so there is nothing to hold.
	func searchProgressively(
		scope: BrowseScope, query: String, limits: SearchLimits
	) -> AsyncStream<MergedResult<LibrarySelection>> {
		AsyncStream { continuation in
			let task = Task {
				let clients = await registry.clientsSnapshot()
				let servers = clients.servers(in: scope)
				let collapse = await settings.mergeDuplicateAlbums
				guard !servers.isEmpty else {
					continuation.yield(MergedResult(items: LibrarySelection()))
					continuation.finish()
					return
				}

				var arrived = [LibrarySelection?](repeating: nil, count: servers.count)
				var failed = [ServerFailure?](repeating: nil, count: servers.count)

				await withTaskGroup(of: (Int, Answer<LibrarySelection>).self) { group in
					for (index, config) in servers.enumerated() {
						guard let client = clients.client(for: config.id) else { continue }
						group.addTask {
							do {
								let found = try await client.search3(
									query: query,
									artistCount: limits.artists,
									albumCount: limits.albums,
									songCount: limits.songs)
								return (
									index,
									.ok(config.id, LibraryMapper.selection(found, server: config.id))
								)
							} catch {
								guard !error.isCancellation else { return (index, .cancelled) }
								return (
									index,
									.failed(
										ServerFailure(
											server: config.id, serverName: config.displayName,
											message: error.userMessage))
								)
							}
						}
					}
					for await (index, answer) in group {
						switch answer {
						case .ok(_, let selection): arrived[index] = selection
						case .failed(let failure): failed[index] = failure
						// A keystroke replaced this query. Recording it would
						// flash a failure note for something nobody is waiting
						// for any more.
						case .cancelled: continue
						}
						continuation.yield(
							Self.combine(arrived, failures: failed, collapse: collapse))
					}
				}
				continuation.finish()
			}
			// Without this, abandoning the stream on the next keystroke leaves
			// the old query's servers running to completion.
			continuation.onTermination = { _ in task.cancel() }
		}
	}

	/// `compactMap` over an index-keyed array, so what survives is still in
	/// registry order.
	private static func combine(
		_ arrived: [LibrarySelection?], failures: [ServerFailure?], collapse: Bool
	) -> MergedResult<LibrarySelection> {
		let selections = arrived.compactMap { $0 }
		let albums = selections.flatMap(\.albums)
		return MergedResult(
			items: LibrarySelection(
				artists: Merge.artists(perServer: selections.map(\.artists)),
				albums: collapse ? Merge.albums(albums) : albums,
				songs: selections.flatMap(\.songs)),
			failures: failures.compactMap { $0 })
	}

	// MARK: - Single-ref reads

	func albumDetail(_ ref: ItemRef) async throws -> AlbumDetail? {
		guard let client = await client(for: ref.server) else { return nil }
		guard let dto = try await client.album(id: ref.id),
			let album = LibraryMapper.album(dto, server: ref.server)
		else {
			return nil
		}
		return AlbumDetail(
			album: album,
			songs: dto.song.compactMap { LibraryMapper.song($0, server: ref.server) })
	}

	func playlist(_ ref: ItemRef) async throws -> Playlist? {
		guard let client = await client(for: ref.server) else { return nil }
		guard let dto = try await client.playlist(id: ref.id) else { return nil }
		return LibraryMapper.playlist(dto, server: ref.server)
	}

	func playlistsOf(server: ServerId) async throws -> [Playlist] {
		guard let client = await client(for: server) else { return [] }
		return try await client.playlists().compactMap { LibraryMapper.playlist($0, server: server) }
	}

	func starred(server: ServerId) async throws -> LibrarySelection {
		guard let client = await client(for: server) else { return LibrarySelection() }
		return LibraryMapper.selection(try await client.starred2(), server: server)
	}

	// MARK: - Prose and extras
	//
	//	Never essential and possibly slow: answering these may send the *server*
	//	out to MusicBrainz and Wikipedia. They return nil on failure rather than
	//	throwing, and **must never be awaited before the thing the user asked
	//	for** — a biography that fails should cost the biography, not the album
	//	list.

	func artistInfo(_ ref: ItemRef) async -> ArtistInfo? {
		guard let client = await client(for: ref.server) else { return nil }
		guard let dto = try? await client.artistInfo2(id: ref.id) else { return nil }
		let info = LibraryMapper.artistInfo(dto)
		return info.isEmpty ? nil : info
	}

	func albumNotes(_ ref: ItemRef) async -> AlbumNotes? {
		guard let client = await client(for: ref.server) else { return nil }
		guard let dto = try? await client.albumInfo2(id: ref.id) else { return nil }
		let notes = LibraryMapper.albumNotes(dto)
		return notes.isEmpty ? nil : notes
	}

	/// How many images the album folder holds, cover included. Drives the hero
	/// pager; a failure means "just the cover, then", not an error.
	func albumImageCount(_ ref: ItemRef) async -> Int {
		guard let client = await client(for: ref.server) else { return 0 }
		return (try? await client.albumImageCount(id: ref.id)) ?? 0
	}

	// MARK: - Writes

	/// **The one write that swallows**, which the rule at the head of this file
	/// allows only because it is stated: a scrobble is not something the user
	/// asked for, so a failed one has no screen to report to and nothing to
	/// retry. Android's `runCatchingCancellable` says the same.
	func scrobble(_ ref: ItemRef, submission: Bool) async {
		guard let client = await client(for: ref.server) else { return }
		try? await client.scrobble(id: ref.id, submission: submission)
	}

	func setStarred(_ ref: ItemRef, kind: StarKind, starred: Bool) async throws {
		guard let client = await client(for: ref.server) else { return }
		let songs = kind == .song ? [ref.id] : []
		let albums = kind == .album ? [ref.id] : []
		let artists = kind == .artist ? [ref.id] : []
		if starred {
			try await client.star(songIds: songs, albumIds: albums, artistIds: artists)
		} else {
			try await client.unstar(songIds: songs, albumIds: albums, artistIds: artists)
		}
	}

	/// `songs` are `ItemRef`s rather than bare ids because **a playlist cannot
	/// hold a track from another server**, and the type is where that is said.
	/// Anything from elsewhere is dropped rather than sent, since the server
	/// would silently ignore it and the playlist would come back shorter than
	/// the user asked for with nothing to explain why.
	@discardableResult
	func createPlaylist(server: ServerId, name: String, songs: [ItemRef]) async throws -> Playlist? {
		guard let client = await client(for: server) else { return nil }
		let created = try await client.createPlaylist(
			name: name, songIds: songs.filter { $0.server == server }.map(\.id))
		await events.playlistsChanged()
		return created.flatMap { LibraryMapper.playlist($0, server: server) }
	}

	func addToPlaylist(_ playlist: ItemRef, song: ItemRef) async throws {
		guard song.server == playlist.server,
			let client = await client(for: playlist.server)
		else { return }
		try await client.updatePlaylist(playlistId: playlist.id, songIdToAdd: [song.id])
		await events.playlistsChanged()
	}

	/// Removes by **position**. The caller must serialise these: positions shift
	/// the moment one is removed, so a second request issued before the first
	/// lands would carry an index the server has already moved and delete the
	/// wrong track.
	func removeFromPlaylist(_ playlist: ItemRef, at index: Int) async throws {
		guard let client = await client(for: playlist.server) else { return }
		try await client.updatePlaylist(playlistId: playlist.id, songIndexToRemove: [index])
		await events.playlistsChanged()
	}

	func deletePlaylist(_ playlist: ItemRef) async throws {
		guard let client = await client(for: playlist.server) else { return }
		try await client.deletePlaylist(id: playlist.id)
		await events.playlistsChanged()
	}

	// MARK: - Covers

	func coverUrls() async -> CoverUrls {
		await registry.clientsSnapshot().coverUrls
	}

	// MARK: - Fan-out machinery

	/// One server's answer.
	///
	/// Deliberately **not** `Result<T, any Error>`: an existential `Error` is
	/// not `Sendable`, so a `Result` carrying one cannot cross a task-group
	/// boundary at all. Converting the error to its user-facing sentence inside
	/// the child task is what makes the value `Sendable` — and it is the right
	/// place for the conversion anyway.
	private enum Answer<Value: Sendable>: Sendable {
		case ok(ServerId, Value)
		case failed(ServerFailure)
		/// Not a failure. A cancelled branch is the user having moved on, and
		/// recording it would flash "this server did not answer" over the
		/// results on every keystroke in search and every change of scope.
		case cancelled
	}

	private struct Gathered<Value: Sendable>: Sendable {
		let answers: [(ServerConfig, Value)]
		let failures: [ServerFailure]
	}

	/// Asks every server in `servers` at once.
	///
	/// In parallel, not in sequence: three servers queried one after another
	/// would make every browse screen as slow as the sum of them, and one that
	/// has gone away would hold up the two that are fine until it times out.
	private func gather<Value: Sendable>(
		over servers: [ServerConfig],
		clients: ServerClients,
		work: @Sendable @escaping (SubsonicClient, ServerConfig) async throws -> Value
	) async -> Gathered<Value> {
		await withTaskGroup(of: Answer<Value>.self) { group in
			for config in servers {
				guard let client = clients.client(for: config.id) else {
					// The configuration is there but the Keychain item is not —
					// what a restore onto a new device looks like when the
					// password did not travel with it. A different sentence
					// from "unreachable", on purpose.
					group.addTask {
						.failed(
							ServerFailure(
								server: config.id, serverName: config.displayName,
								message: "No saved password. Open the server in Settings."))
					}
					continue
				}
				group.addTask {
					do {
						return .ok(config.id, try await work(client, config))
					} catch {
						guard !error.isCancellation else { return .cancelled }
						return .failed(
							ServerFailure(
								server: config.id, serverName: config.displayName,
								message: error.userMessage))
					}
				}
			}
			var collected: [Answer<Value>] = []
			for await answer in group { collected.append(answer) }
			return Self.assemble(collected, order: servers)
		}
	}

	/// The same fan-out driven by refs rather than by scope, for a merged row
	/// that names a different id on each of its servers.
	///
	/// A ref whose server has since been removed is skipped rather than
	/// reported: that is stale navigation state, not a server that failed.
	private func gather<Value: Sendable>(
		over refs: [ItemRef],
		clients: ServerClients,
		work: @Sendable @escaping (SubsonicClient, ItemRef) async throws -> Value
	) async -> Gathered<Value> {
		// One branch per server, not per ref: a merged row holds one ref per
		// contributing server, and asking the same server twice would double
		// its albums. The refs arrive in registry order because that is the
		// order they were merged in, so the order survives.
		var seen: Set<ServerId> = []
		var targets: [(ServerConfig, SubsonicClient, ItemRef)] = []
		for ref in refs where !seen.contains(ref.server) {
			guard let config = clients.config(for: ref.server),
				let client = clients.client(for: ref.server)
			else { continue }
			seen.insert(ref.server)
			targets.append((config, client, ref))
		}

		let answers = await withTaskGroup(of: Answer<Value>.self) { group in
			for (config, client, ref) in targets {
				group.addTask {
					do {
						return .ok(config.id, try await work(client, ref))
					} catch {
						guard !error.isCancellation else { return .cancelled }
						return .failed(
							ServerFailure(
								server: config.id, serverName: config.displayName,
								message: error.userMessage))
					}
				}
			}
			var collected: [Answer<Value>] = []
			for await answer in group { collected.append(answer) }
			return collected
		}
		return Self.assemble(answers, order: targets.map(\.0))
	}

	/// Reassembles a group's answers **in registry order, not completion
	/// order**.
	///
	/// This is the one place Swift and Kotlin differ dangerously. Kotlin's
	/// `awaitAll()` preserves the input order for free; `for await … in group`
	/// yields as each child finishes. Taking the group's order would make the
	/// merge tie-break — and so which server's artwork and index letter a
	/// merged row takes — depend on which server happened to answer first, and
	/// the list would visibly reshuffle whenever one was slow. It looks correct
	/// in every single-server and every fast-LAN test.
	private static func assemble<Value: Sendable>(
		_ answers: [Answer<Value>], order: [ServerConfig]
	) -> Gathered<Value> {
		var ok: [ServerId: Value] = [:]
		var bad: [ServerId: ServerFailure] = [:]
		for answer in answers {
			switch answer {
			case .ok(let id, let value): ok[id] = value
			case .failed(let failure): bad[failure.server] = failure
			case .cancelled: break
			}
		}
		return Gathered(
			answers: order.compactMap { config in ok[config.id].map { (config, $0) } },
			failures: order.compactMap { bad[$0.id] })
	}

	/// A heading over nothing is noise, so a server that contributed no rows
	/// gets no section.
	private func sections<Item: Sendable>(_ gathered: Gathered<[Item]>) -> [ServerSection<Item>] {
		gathered.answers.filter { !$0.1.isEmpty }.map { ServerSection(server: $0.0, items: $0.1) }
	}

	private func client(for server: ServerId) async -> SubsonicClient? {
		await registry.client(for: server)
	}
}

/// How many of each kind to ask for. Zero asks the server to skip that
/// category entirely, which is what a switched-off filter chip means.
struct SearchLimits: Sendable {
	var artists = 20
	var albums = 30
	var songs = 60
}
