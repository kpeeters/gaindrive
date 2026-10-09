//	GainDrive for iOS
//	Copyright (C) 2026 Kasper Peeters
//
//	This file is part of GainDrive, distributed under the GNU General
//	Public License version 3 or later, with the Application Distribution
//	Exception in ios/LICENSE. See LICENSE at the repository root for the
//	full text of the GPL.

import Foundation

/// The downloaded films on disk, and their captions.
///
/// **Not part of `AudioStore`.** A film arrives as an HLS stream saved by
/// `AVAssetDownloadURLSession`, which writes a `.movpkg` package wherever the
/// system chooses; only its location is ours to keep. So this is an index of
/// locations, plus the captions, which the package cannot hold because ours
/// are fetched separately rather than listed in the playlist.
///
/// Locations are kept **relative to the home directory**: the app container's
/// absolute path changes across updates and reinstalls, and a stored absolute
/// path would then name nothing.
///
/// Every film here is pinned - nothing is kept from playing - so there is no
/// eviction, only removal.
actor FilmStore {
	static let shared = FilmStore()

	private struct Entry: Codable {
		var ref: ItemRef
		var path: String
		/// False while the download is in flight: the package exists from
		/// the first segment, and must not be played or listed until the
		/// session says it is whole.
		var complete: Bool
	}

	private struct StoredCaption: Codable {
		var id: String
		var name: String
		var file: String
	}

	private let root: URL
	private let indexFile: URL
	private var entries: [ItemRef: Entry] = [:]
	private var didChange: (@Sendable () -> Void)?

	init() {
		let base = FileManager.default.urls(for: .applicationSupportDirectory, in: .userDomainMask)[0]
		root = base.appending(path: "Films", directoryHint: .isDirectory)
		indexFile = root.appending(path: "index.json")
		try? FileManager.default.createDirectory(at: root, withIntermediateDirectories: true)
		var url = root
		var values = URLResourceValues()
		values.isExcludedFromBackup = true
		try? url.setResourceValues(values)
		if let data = try? Data(contentsOf: indexFile),
			let list = try? JSONDecoder().decode([Entry].self, from: data)
		{
			entries = Dictionary(list.map { ($0.ref, $0) }, uniquingKeysWith: { $1 })
		}
	}

	func setChangeHandler(_ handler: @escaping @Sendable () -> Void) {
		didChange = handler
	}

	// MARK: - Packages

	/// Where the session is writing. Called as soon as it knows, so a removal
	/// mid-download can delete the partial package; a different earlier
	/// location for the same film is a restarted download, and goes.
	func begin(_ ref: ItemRef, at location: URL) {
		let path = Self.relative(location)
		if let old = entries[ref], old.path != path {
			try? FileManager.default.removeItem(at: Self.absolute(old.path))
		}
		entries[ref] = Entry(ref: ref, path: path, complete: false)
		persist()
	}

	func complete(_ ref: ItemRef) {
		guard entries[ref] != nil else { return }
		entries[ref]?.complete = true
		persist()
		didChange?()
	}

	/// A download that failed: its partial package is worth nothing.
	func discard(_ ref: ItemRef) {
		guard let entry = entries[ref], !entry.complete else { return }
		try? FileManager.default.removeItem(at: Self.absolute(entry.path))
		entries[ref] = nil
		persist()
	}

	func location(for ref: ItemRef) -> URL? {
		guard let entry = entries[ref], entry.complete else { return nil }
		let url = Self.absolute(entry.path)
		return FileManager.default.fileExists(atPath: url.path(percentEncoded: false)) ? url : nil
	}

	func heldRefs() -> Set<ItemRef> {
		Set(entries.values.filter(\.complete).map(\.ref))
	}

	func remove(_ ref: ItemRef) {
		if let entry = entries[ref] {
			try? FileManager.default.removeItem(at: Self.absolute(entry.path))
			entries[ref] = nil
			persist()
		}
		try? FileManager.default.removeItem(at: captionDirectory(for: ref))
		didChange?()
	}

	func removeAll() {
		for entry in entries.values {
			try? FileManager.default.removeItem(at: Self.absolute(entry.path))
		}
		entries = [:]
		try? FileManager.default.removeItem(at: root)
		try? FileManager.default.createDirectory(at: root, withIntermediateDirectories: true)
		persist()
		didChange?()
	}

	/// Partial packages included: they take the room all the same.
	func totalBytes() -> Int64 {
		entries.values.reduce(Self.size(of: root)) { $0 + Self.size(of: Self.absolute($1.path)) }
	}

	// MARK: - Captions

	/// Nil when nothing was stored, which is not the same as a film with no
	/// captions: that stores an empty list, so it is not asked again.
	func captions(for ref: ItemRef) -> [CaptionTrack]? {
		guard let list = storedCaptions(for: ref) else { return nil }
		return list.map { CaptionTrack(id: $0.id, name: $0.name) }
	}

	func captionText(for ref: ItemRef, id: String) -> String? {
		guard let caption = storedCaptions(for: ref)?.first(where: { $0.id == id }) else {
			return nil
		}
		let url = captionDirectory(for: ref).appending(path: caption.file)
		return try? String(contentsOf: url, encoding: .utf8)
	}

	func saveCaptions(_ tracks: [(CaptionTrack, String)], for ref: ItemRef) {
		let directory = captionDirectory(for: ref)
		try? FileManager.default.createDirectory(at: directory, withIntermediateDirectories: true)
		var list: [StoredCaption] = []
		for (index, (track, text)) in tracks.enumerated() {
			// Numbered rather than named by id: a sidecar's id is "-1".
			let file = "\(index).vtt"
			guard (try? text.write(
				to: directory.appending(path: file), atomically: true, encoding: .utf8)) != nil
			else { continue }
			list.append(StoredCaption(id: track.id, name: track.name, file: file))
		}
		guard let data = try? JSONEncoder().encode(list) else { return }
		try? data.write(to: directory.appending(path: "captions.json"), options: .atomic)
	}

	private func storedCaptions(for ref: ItemRef) -> [StoredCaption]? {
		let url = captionDirectory(for: ref).appending(path: "captions.json")
		guard let data = try? Data(contentsOf: url) else { return nil }
		return try? JSONDecoder().decode([StoredCaption].self, from: data)
	}

	private func captionDirectory(for ref: ItemRef) -> URL {
		root
			.appending(path: FileNames.component(ref.server.description))
			.appending(path: FileNames.component(ref.id))
	}

	// MARK: - Machinery

	private func persist() {
		guard let data = try? JSONEncoder().encode(Array(entries.values)) else { return }
		try? data.write(to: indexFile, options: .atomic)
	}

	private static var home: URL { URL(filePath: NSHomeDirectory(), directoryHint: .isDirectory) }

	/// Compared by components with symlinks resolved: on iOS the container
	/// is reached as both `/var/...` and `/private/var/...`.
	private static func relative(_ url: URL) -> String {
		let full = url.resolvingSymlinksInPath().pathComponents
		let base = home.resolvingSymlinksInPath().pathComponents
		guard full.starts(with: base) else { return url.path(percentEncoded: false) }
		return "~/" + full.dropFirst(base.count).joined(separator: "/")
	}

	private static func absolute(_ path: String) -> URL {
		guard path.hasPrefix("~/") else { return URL(filePath: path) }
		return home.appending(path: String(path.dropFirst(2)))
	}

	/// A package is a directory, so its size is a walk.
	private static func size(of url: URL) -> Int64 {
		guard
			let walk = FileManager.default.enumerator(
				at: url, includingPropertiesForKeys: [.fileSizeKey], options: [])
		else { return 0 }
		var total: Int64 = 0
		for case let file as URL in walk {
			total += Int64((try? file.resourceValues(forKeys: [.fileSizeKey]).fileSize) ?? 0)
		}
		return total
	}
}
