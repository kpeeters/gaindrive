//	GainDrive for iOS
//	Copyright (C) 2026 Kasper Peeters
//
//	This file is part of GainDrive, distributed under the GNU General
//	Public License version 3 or later, with the Application Distribution
//	Exception in ios/LICENSE. See LICENSE at the repository root for the
//	full text of the GPL.

import Foundation

/// What each browse query last answered, so a server that is not there can
/// still be read.
///
/// **Network-first with a local fallback**, not stale-while-revalidate. That
/// delivers offline browsing without turning every read in `LibraryRepository`
/// into a stream and rewriting every view model to match; moving to
/// emit-stored-then-refresh later touches that class and the view models and
/// nothing else. `android/CACHING.md` reaches the same conclusion for the same
/// reason.
///
/// **A file per query, not a database**, which is a departure from the plan and
/// from Android's Room mirror. Two things settled it. The mirror is a cache of
/// *answers* — each browse call has a natural key, and storing the mapped
/// domain value under it is the whole of the write path — so the relational
/// shape buys nothing that is used. And it makes the migration story free: a
/// value written by a build whose models had a different shape fails to decode,
/// which reads as a miss, which is a re-fetch. That is exactly the destructive
/// migration the plan asked for, without a schema to destroy.
///
/// What a database *would* buy is queries across the mirror, and there is one
/// that will want it: "which albums have any stored audio", for the offline
/// listing filter. That is why this exposes `songs(of:)` and `albums(of:)`
/// rather than only opaque blobs — the walk is possible, and whether it is fast
/// enough is a measurement for when the filter is built.
///
/// Rows are keyed on `(serverId, …)` throughout, never on a bare Subsonic id.
/// The composite identity rule that holds everywhere else has to hold in
/// storage too, and the directory layout is what enforces it: removing a server
/// is removing a directory.
actor LibraryMirror {
	/// What a stored answer belongs to.
	///
	/// The mode is part of the artist-index key because a listing must never
	/// mix slices: Categories stored over Artists would be a chip that shows
	/// the wrong library the moment its server goes away.
	enum Key: Sendable {
		case indexes(ServerId, LibraryMode)
		case chips(ServerId)
		case playlists(ServerId)
		case albums(ItemRef)
		case album(ItemRef)

		var server: ServerId {
			switch self {
			case .indexes(let server, _), .chips(let server), .playlists(let server):
				server
			case .albums(let ref), .album(let ref):
				ref.server
			}
		}

		var name: String {
			switch self {
			case .indexes(_, let mode): "indexes-\(FileNames.component(mode.id))"
			case .chips: "chips"
			case .playlists: "playlists"
			case .albums(let ref): "albums-\(FileNames.component(ref.id))"
			case .album(let ref): "album-\(FileNames.component(ref.id))"
			}
		}
	}

	/// The way **up** the tree, recorded on the way down.
	///
	/// Offline, the question is "which albums have any stored audio", and the
	/// cheap way to answer it is to start from the files that are actually
	/// here — a few hundred at most — and walk up, rather than walking down
	/// through every album the mirror holds asking whether any of its tracks
	/// landed. Going up needs a reverse map, and the moment to build one is
	/// while the album is being mirrored anyway.
	///
	/// Keyed by `ItemRef.encoded` rather than by `ItemRef`, because a
	/// dictionary whose key is not a `String` encodes as an unkeyed array of
	/// alternating keys and values — which round-trips, and is unreadable.
	struct Availability: Codable, Sendable {
		var albumOfSong: [String: ItemRef] = [:]
		var artistOfAlbum: [String: ItemRef] = [:]

		/// Everything reachable from what is on disk.
		///
		/// Two dictionary lookups per stored file and nothing else — no album
		/// is opened, and an album whose tracks were never mirrored simply does
		/// not appear, which is right: it cannot be listed either.
		func reachable(from held: Set<ItemRef>) -> (albums: Set<ItemRef>, artists: Set<ItemRef>) {
			var albums: Set<ItemRef> = []
			var artists: Set<ItemRef> = []
			for song in held {
				guard let album = albumOfSong[song.encoded] else { continue }
				albums.insert(album)
				if let artist = artistOfAlbum[album.encoded] { artists.insert(artist) }
			}
			return (albums, artists)
		}

		mutating func merge(_ other: Availability) {
			albumOfSong.merge(other.albumOfSong) { _, new in new }
			artistOfAlbum.merge(other.artistOfAlbum) { _, new in new }
		}
	}

	private let root: URL

	init(root: URL? = nil) {
		if let root {
			self.root = root
		} else {
			let base = FileManager.default.urls(
				for: .applicationSupportDirectory, in: .userDomainMask)[0]
			self.root = base.appending(path: "Mirror", directoryHint: .isDirectory)
		}
		var url = self.root
		try? FileManager.default.createDirectory(at: url, withIntermediateDirectories: true)
		// Excluded from backup for the same reason the audio is: it is all
		// re-fetchable, and a mirror of a large library would dominate one.
		var values = URLResourceValues()
		values.isExcludedFromBackup = true
		try? url.setResourceValues(values)
	}

	func store<Value: Encodable & Sendable>(_ value: Value, at key: Key) {
		guard let data = try? JSONEncoder().encode(value) else { return }
		let url = self.url(for: key)
		try? FileManager.default.createDirectory(
			at: url.deletingLastPathComponent(), withIntermediateDirectories: true)
		// Atomic, so a crash mid-write leaves the previous answer rather than
		// half of the new one. A truncated file would decode to nothing, which
		// is survivable — but a browse that silently lost a server's library
		// because the app was killed is not worth the saved syscall.
		try? data.write(to: url, options: .atomic)
	}

	func load<Value: Decodable & Sendable>(_ type: Value.Type, at key: Key) -> Value? {
		guard let data = try? Data(contentsOf: url(for: key)) else { return nil }
		return try? JSONDecoder().decode(Value.self, from: data)
	}

	/// An album and its tracks, plus the step up that offline availability
	/// needs.
	///
	/// A method of its own rather than the generic `store`, so the blob and the
	/// index cannot be written apart — a stored album whose tracks are not in
	/// the index is an album that never appears offline, silently.
	func storeAlbum(_ detail: AlbumDetail, for ref: ItemRef) {
		store(detail, at: .album(ref))
		var index = availability(for: ref.server)
		for song in detail.songs {
			index.albumOfSong[song.ref.encoded] = ref
		}
		if let artist = detail.album.artistRef {
			index.artistOfAlbum[ref.encoded] = artist
		}
		write(index, for: ref.server)
	}

	/// An artist's albums, and the step up from each of them.
	///
	/// The artist ref is taken from the *request* rather than from
	/// `Album.artistRef`: the directory-shaped listings carry only `parent`,
	/// and an album reached that way would otherwise record a step up to
	/// nothing.
	func storeAlbums(_ albums: [Album], for artist: ItemRef) {
		store(albums, at: .albums(artist))
		var index = availability(for: artist.server)
		for album in albums {
			index.artistOfAlbum[album.ref.encoded] = artist
		}
		write(index, for: artist.server)
	}

	func availability(for server: ServerId) -> Availability {
		guard let data = try? Data(contentsOf: availabilityURL(for: server)),
			let index = try? JSONDecoder().decode(Availability.self, from: data)
		else { return Availability() }
		return index
	}

	/// Across every server, which is what a merged listing is filtered against.
	func availability(for servers: [ServerId]) -> Availability {
		var merged = Availability()
		for server in servers { merged.merge(availability(for: server)) }
		return merged
	}

	private func write(_ index: Availability, for server: ServerId) {
		guard let data = try? JSONEncoder().encode(index) else { return }
		let url = availabilityURL(for: server)
		try? FileManager.default.createDirectory(
			at: url.deletingLastPathComponent(), withIntermediateDirectories: true)
		try? data.write(to: url, options: .atomic)
	}

	private func availabilityURL(for server: ServerId) -> URL {
		directory(for: server).appending(path: "availability.json")
	}

	/// Removing a server drops its rows, which is a directory removal because
	/// the layout is keyed that way.
	func forget(_ server: ServerId) {
		try? FileManager.default.removeItem(at: directory(for: server))
	}

	func forgetEverything() {
		try? FileManager.default.removeItem(at: root)
		try? FileManager.default.createDirectory(at: root, withIntermediateDirectories: true)
	}

	private func directory(for server: ServerId) -> URL {
		root.appending(path: FileNames.component(server.description))
	}

	private func url(for key: Key) -> URL {
		directory(for: key.server).appending(path: key.name + ".json")
	}
}
