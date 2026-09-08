//	GainDrive for iOS
//	Copyright (C) 2026 Kasper Peeters
//
//	This file is part of GainDrive, distributed under the GNU General
//	Public License version 3 or later, with the Application Distribution
//	Exception in ios/LICENSE. See LICENSE at the repository root for the
//	full text of the GPL.

import Foundation

/// The downloaded audio on disk.
///
/// **Under Application Support, not Caches.** The system purges `Caches` under
/// storage pressure, which would delete downloads at exactly the moment
/// somebody is offline and relying on them — the same reasoning that put
/// Android's in `filesDir` rather than `cacheDir`. Excluded from backup, since
/// it is all re-fetchable and a music library would dominate one.
///
/// Laid out as `Media/<serverId>/<songId>@<qualityTag>`, which is
/// `CacheKeys.of` with no rearranging: the key's own separator is the server
/// directory. That keeps one definition of the key rather than a second one
/// for filenames, and it makes dropping a removed server a directory removal.
///
/// In stage 2 this becomes the backing store of the byte cache as well. Today
/// nothing is written except a completed download, which is why there is no
/// eviction here: with nothing unpinned on disk there is no victim to pick.
actor AudioStore {
	private let root: URL

	init(root: URL? = nil) {
		if let root {
			self.root = root
		} else {
			let base = FileManager.default.urls(
				for: .applicationSupportDirectory, in: .userDomainMask)[0]
			self.root = base.appending(path: "Media", directoryHint: .isDirectory)
		}
		prepare()
	}

	/// Where a download at this quality belongs, **before its extension**.
	/// `adopt` adds that; see `AudioFormat.fileExtension` for why a stored file
	/// must have one.
	nonisolated private func base(for ref: ItemRef, quality: AudioQuality) -> URL {
		Self.path(root: root, key: CacheKeys.of(ref, quality: quality))
	}

	/// **Any stored copy of this song, whatever quality it is.**
	///
	/// The quality is in the key so two copies can coexist and neither is ever
	/// mislabelled — but playback wants the music, and a copy pinned at one
	/// setting must not stop being playable because the setting changed. That
	/// is `android/CACHING.md`'s "copies already on the device stay playable",
	/// reached here without an evictor to reclaim them.
	func storedFile(for ref: ItemRef) -> URL? {
		copies(of: ref).sorted { $0.lastPathComponent < $1.lastPathComponent }.first
	}

	func isStored(_ ref: ItemRef) -> Bool {
		storedFile(for: ref) != nil
	}

	/// A concrete `Set` rather than `some Sequence<ItemRef>`: an opaque
	/// parameter carries no `Sendable` bound, and this is called across an
	/// actor boundary.
	func storedRefs(among refs: Set<ItemRef>) -> Set<ItemRef> {
		refs.filter { isStored($0) }
	}

	/// Moves a finished download into place.
	///
	/// **A rename is the only publish**, as in the server's `TranscodeCache`:
	/// nothing is ever written in place, so a half-finished download cannot be
	/// mistaken for a playable track. `URLSession` hands over a complete
	/// temporary file, so the rename is all that is needed.
	///
	/// **`nonisolated`, and that is load-bearing.** `URLSession` deletes the
	/// temporary file the moment its delegate returns, so awaiting an actor
	/// before the rename would reliably lose every download — and only on a
	/// device slow enough to notice, which is the worst way to find out.
	nonisolated func adopt(
		_ temporary: URL, for ref: ItemRef, quality: AudioQuality, fileExtension: String
	) throws {
		let manager = FileManager.default
		// **The extension is not decoration.** AVFoundation types a local file
		// by its path extension and has no header to fall back on, so a file
		// without one is never reported as unplayable — the player just waits.
		let destination = base(for: ref, quality: quality)
			.appendingPathExtension(fileExtension)
		try manager.createDirectory(
			at: destination.deletingLastPathComponent(), withIntermediateDirectories: true)
		try? manager.removeItem(at: destination)
		try manager.moveItem(at: temporary, to: destination)
	}

	/// Every copy of the song, whatever quality. Unpinning removes the bytes —
	/// it is what "remove download" is taken to mean, and orphaned files that
	/// nothing lists are worse.
	func remove(_ ref: ItemRef) {
		for copy in copies(of: ref) {
			try? FileManager.default.removeItem(at: copy)
		}
	}

	/// **URLs throughout, never `contentsOfDirectory(atPath:)`.** A file URL's
	/// `path()` is percent-encoded while the filesystem name is not, so mixing
	/// the two compares an encoded prefix against a decoded name and silently
	/// finds nothing. `lastPathComponent` is the decoded name, which is what
	/// `component(_:)` produced on the way in.
	private func copies(of ref: ItemRef) -> [URL] {
		let directory = root.appending(path: Self.component(ref.server.description))
		let prefix = Self.component(ref.id) + "@"
		let found =
			(try? FileManager.default.contentsOfDirectory(
				at: directory, includingPropertiesForKeys: nil)) ?? []
		return found.filter { $0.lastPathComponent.hasPrefix(prefix) }
	}

	func removeAll() {
		try? FileManager.default.removeItem(at: root)
		prepare()
	}

	func totalBytes() -> Int64 {
		guard
			let walk = FileManager.default.enumerator(
				at: root, includingPropertiesForKeys: [.fileSizeKey], options: [.skipsHiddenFiles])
		else { return 0 }
		var total: Int64 = 0
		for case let url as URL in walk {
			let size = (try? url.resourceValues(forKeys: [.fileSizeKey]).fileSize) ?? 0
			total += Int64(size)
		}
		return total
	}

	/// Bumped when the **layout on disk** changes in a way that makes what is
	/// already there unusable, which drops it once on open.
	///
	/// Version 1 stored files with no extension, and every one of them hung the
	/// player. There is no repairing those in place without knowing what
	/// container each holds, and re-downloading is cheap — the pins survive, so
	/// `refresh()` fetches them again. This is the server's
	/// `MUSIC_CACHE_VERSION` for the same reason: a cache is allowed to be
	/// thrown away, and being wrong about one costs a re-fetch.
	private static let layoutVersion = 2

	nonisolated private func prepare() {
		try? FileManager.default.createDirectory(at: root, withIntermediateDirectories: true)
		migrate()
		var url = root
		var values = URLResourceValues()
		values.isExcludedFromBackup = true
		try? url.setResourceValues(values)
	}

	/// Drops everything stored under an older layout, once.
	nonisolated private func migrate() {
		let marker = root.appending(path: ".layout")
		let found = (try? String(contentsOf: marker, encoding: .utf8)).flatMap(Int.init)
		guard found != Self.layoutVersion else { return }
		if let contents = try? FileManager.default.contentsOfDirectory(
			at: root, includingPropertiesForKeys: nil)
		{
			for item in contents { try? FileManager.default.removeItem(at: item) }
		}
		try? String(Self.layoutVersion).write(to: marker, atomically: true, encoding: .utf8)
	}

	/// **Percent-encoded per component**, and not because gaindrive needs it:
	/// its ids are integers and its server ids are UUIDs. A Subsonic id is a
	/// string somebody else chose, and one containing a slash would otherwise
	/// write outside the directory it was meant for.
	///
	/// Hyphens, dots and underscores are left alone so a UUID directory and an
	/// integer filename read as themselves — an escaped hyphen in every server
	/// directory would be noise in every `ls` for no gain. What matters is only
	/// that the transformation is the same on the way in and on the way out.
	static func component(_ raw: String) -> String {
		raw.addingPercentEncoding(withAllowedCharacters: Self.nameSafe) ?? raw
	}

	private static let nameSafe: CharacterSet = {
		var set = CharacterSet.alphanumerics
		set.insert(charactersIn: "-._")
		return set
	}()

	static func path(root: URL, key: String) -> URL {
		guard let parsed = CacheKeys.parse(key) else {
			return root.appending(path: component(key))
		}
		return
			root
			.appending(path: component(parsed.ref.server.description))
			.appending(path: component(parsed.ref.id) + "@" + parsed.quality.tag)
	}
}
