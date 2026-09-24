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
/// somebody is offline and relying on them - the same reasoning that put
/// Android's in `filesDir` rather than `cacheDir`. Excluded from backup, since
/// it is all re-fetchable and a music library would dominate one.
///
/// Laid out as `Media/<serverId>/<songId>@<qualityTag>`, which is
/// `CacheKeys.of` with no rearranging: the key's own separator is the server
/// directory. That keeps one definition of the key rather than a second one
/// for filenames, and it makes dropping a removed server a directory removal.
///
/// It backs both halves of the cache: a pinned download, and a track kept
/// because it was played. Only completed files are ever visible - a `.part`
/// file belongs to a fetch in flight and is adopted or discarded, never
/// listed.
actor AudioStore {
	private let root: URL
	/// Pushed in by `PinRepository`, which is the only thing that knows both.
	///
	/// **The store is told what is protected; it does not ask.** It knows
	/// nothing about pins and should not learn - the set is `Pins.expand`,
	/// computed where pins live, handed over as a plain set.
	private var cap: Int64 = .max
	private var protected: Set<ItemRef> = []
	/// Fired whenever what is held changes, so the marks on screen can follow a
	/// track that arrived or was evicted while they were being looked at.
	///
	/// A callback rather than the store knowing who cares: it is set once, by
	/// the composition root, for the same reason
	/// `ServerRegistry.onServerInvalidated` is.
	private var didChange: (@Sendable () -> Void)?

	func setChangeHandler(_ handler: @escaping @Sendable () -> Void) {
		didChange = handler
	}

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

	func setLimits(cap: Int64, protecting refs: Set<ItemRef>) {
		self.cap = cap
		protected = refs
	}

	/// Everything held, whatever put it there.
	///
	/// Walked rather than kept as a running set. The set would have to be
	/// updated from `adopt`, which is `nonisolated` and cannot touch actor
	/// state, and from eviction - two places to forget. A directory of a few
	/// hundred files is not worth that.
	func heldRefs() -> Set<ItemRef> {
		guard
			let walk = FileManager.default.enumerator(
				at: root, includingPropertiesForKeys: nil, options: [.skipsHiddenFiles])
		else { return [] }
		var found: Set<ItemRef> = []
		for case let url as URL in walk where url.pathExtension != "part" {
			if let ref = Self.ref(of: url) { found.insert(ref) }
		}
		return found
	}

	/// Called after a `nonisolated` adopt, which is the one write that cannot
	/// do this itself.
	func finishedAdopting() {
		evict()
		didChange?()
	}

	/// The inverse of the layout: `<serverId>/<songId>@<tag>[.ext]`.
	///
	/// Both components were percent-encoded on the way in, so both are decoded
	/// on the way out - the transformation has to be the same in both
	/// directions or a track reads as absent the moment its id is anything but
	/// digits.
	static func ref(of url: URL) -> ItemRef? {
		let name = url.deletingPathExtension().lastPathComponent
		guard let at = name.lastIndex(of: "@") else { return nil }
		let id = String(name[name.startIndex..<at]).removingPercentEncoding
		let directory = url.deletingLastPathComponent().lastPathComponent
		guard let id, !id.isEmpty,
			let server = directory.removingPercentEncoding,
			let uuid = UUID(uuidString: server)
		else { return nil }
		return ItemRef(server: ServerId(uuid), id: id)
	}

	/// Where the resource loader writes while a track is arriving.
	///
	/// Beside the finished file rather than in a temporary directory, so a
	/// crash leaves it where the next `PartialFile` for the same track will
	/// truncate it rather than somewhere nothing will ever look. It is not
	/// adopted unless it completed, so it can never be mistaken for a track.
	nonisolated func partURL(for ref: ItemRef, quality: AudioQuality) -> URL {
		base(for: ref, quality: quality).appendingPathExtension("part")
	}

	/// **Any stored copy of this song, whatever quality it is.**
	///
	/// The quality is in the key so two copies can coexist and neither is ever
	/// mislabelled - but playback wants the music, and a copy pinned at one
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
	/// before the rename would reliably lose every download - and only on a
	/// device slow enough to notice, which is the worst way to find out.
	nonisolated func adopt(
		_ temporary: URL, for ref: ItemRef, quality: AudioQuality, fileExtension: String
	) throws {
		let manager = FileManager.default
		// **The extension is not decoration.** AVFoundation types a local file
		// by its path extension and has no header to fall back on, so a file
		// without one is never reported as unplayable - the player just waits.
		let destination = base(for: ref, quality: quality)
			.appendingPathExtension(fileExtension)
		try manager.createDirectory(
			at: destination.deletingLastPathComponent(), withIntermediateDirectories: true)
		try? manager.removeItem(at: destination)
		try manager.moveItem(at: temporary, to: destination)
	}

	/// Every copy of the song, whatever quality. Unpinning removes the bytes -
	/// it is what "remove download" is taken to mean, and orphaned files that
	/// nothing lists are worse.
	func remove(_ ref: ItemRef) {
		for copy in copies(of: ref) {
			try? FileManager.default.removeItem(at: copy)
		}
		didChange?()
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
		didChange?()
	}

	/// What eviction cannot reclaim, which is what the pin cap is really
	/// against: unpinned bytes give way, pinned ones do not.
	func protectedBytes() -> Int64 {
		protected.reduce(Int64(0)) { total, ref in
			total + copies(of: ref).reduce(Int64(0)) { $0 + Self.size(of: $1) }
		}
	}

	/// Least recently modified first, pinned never, stop once under the cap.
	///
	/// Run after an adopt rather than on a timer: the only thing that grows
	/// this directory is a file landing in it, so that is the moment to look.
	func evict() {
		let protectedFiles = Set(protected.flatMap { copies(of: $0) })
		var candidates: [Victim] = []
		var total: Int64 = 0
		guard
			let walk = FileManager.default.enumerator(
				at: root, includingPropertiesForKeys: Self.evictionKeys,
				options: [.skipsHiddenFiles])
		else { return }
		for case let url as URL in walk {
			// A part file belongs to a fetch in flight. It is not a track and
			// deleting it underneath the loader writing to it would be the one
			// way to produce a truncated file that looks whole.
			guard url.pathExtension != "part" else { continue }
			let size = Self.size(of: url)
			total += size
			guard !protectedFiles.contains(url) else { continue }
			candidates.append(Victim(url: url, size: size, modified: Self.modified(of: url)))
		}
		let losers = Self.victims(candidates, total: total, cap: cap)
		for url in losers {
			try? FileManager.default.removeItem(at: url)
		}
		if !losers.isEmpty { didChange?() }
	}

	struct Victim: Sendable {
		let url: URL
		let size: Int64
		let modified: Date
	}

	/// Pure, so the order can be tested without a disk.
	static func victims(_ candidates: [Victim], total: Int64, cap: Int64) -> [URL] {
		guard total > cap else { return [] }
		var over = total - cap
		var chosen: [URL] = []
		for victim in candidates.sorted(by: { $0.modified < $1.modified }) {
			guard over > 0 else { break }
			chosen.append(victim.url)
			over -= victim.size
		}
		return chosen
	}

	private static let evictionKeys: [URLResourceKey] = [
		.fileSizeKey, .contentModificationDateKey,
	]

	private static func size(of url: URL) -> Int64 {
		Int64((try? url.resourceValues(forKeys: [.fileSizeKey]).fileSize) ?? 0)
	}

	/// Modification rather than access: `atime` is unreliable under the
	/// filesystem's own optimisations, which is the same reason the server's
	/// `TranscodeCache` evicts by `mtime`. A file is touched when it is
	/// adopted, so "least recently modified" is "least recently arrived" -
	/// which is not quite "least recently played", and is the honest
	/// approximation until something records a play.
	private static func modified(of url: URL) -> Date {
		(try? url.resourceValues(forKeys: [.contentModificationDateKey]).contentModificationDate)
			?? .distantPast
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
	/// container each holds, and re-downloading is cheap - the pins survive, so
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

	/// Shared with the mirror - see `FileNames`, which says why there is one
	/// definition of this and not two.
	static func component(_ raw: String) -> String {
		FileNames.component(raw)
	}

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
