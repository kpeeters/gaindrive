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
