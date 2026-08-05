//	GainDrive for iOS
//	Copyright (C) 2026 Kasper Peeters
//
//	This file is part of GainDrive, distributed under the GNU General
//	Public License version 3 or later, with the Application Distribution
//	Exception in ios/LICENSE. See LICENSE at the repository root for the
//	full text of the GPL.

import CryptoKit
import Foundation
import UIKit

/// Cover art: memory cache, disk cache, and one request per image no matter how
/// many rows ask for it.
///
/// **Why this is written rather than taken from a package.** `PLAN.md` left
/// Nuke against Kingfisher open, with "which points at a shared `URLSession`
/// most cleanly" as the deciding factor — and neither does, since both build
/// their own session from a configuration. The sharper reason is the cache key.
/// A cover URL carries `t=` and `s=`, and the salt is regenerated every launch,
/// so *any* URL-keyed cache — `URLCache` included — sees a different key for
/// the same bytes on every cold start and re-downloads the whole grid. Owning
/// the loader means owning the key, and `CoverSource.cacheKey` names what the
/// bytes are rather than how they were fetched.
///
/// The disk half is therefore not an optimisation over `URLCache`; it is the
/// only place the stable key can be honoured. `HTTP.shared` still does the
/// fetching, so the connection pool really is shared with the API client.
actor ImageStore {
	/// A single shared instance, as `HTTP.shared` is. This is a cache rather
	/// than a dependency — there is exactly one, it lives as long as the
	/// process, and nothing about it varies per server or per user — so
	/// threading it through the environment would be ceremony around a global
	/// that is already global.
	static let shared = ImageStore()

	/// `NSCache` rather than a dictionary: it evicts under memory pressure on
	/// its own, which a scrolling grid of artwork will eventually need.
	private let memory = NSCache<NSString, UIImage>()
	private let directory: URL
	private let session: URLSession

	/// Requests in flight, keyed by cache key. A grid scrolled quickly asks for
	/// the same cover from several cells at once; without this each would issue
	/// its own request for bytes the others are already fetching.
	private var inFlight: [String: Task<UIImage?, Never>] = [:]

	private static let diskLimit = 128 << 20

	init(session: URLSession = HTTP.shared) {
		self.session = session
		let caches = FileManager.default.urls(for: .cachesDirectory, in: .userDomainMask)[0]
		directory = caches.appending(path: "covers", directoryHint: .isDirectory)
		try? FileManager.default.createDirectory(at: directory, withIntermediateDirectories: true)
		memory.countLimit = 400
	}

	func image(for source: CoverSource) async -> UIImage? {
		if let cached = memory.object(forKey: source.cacheKey as NSString) {
			return cached
		}
		if let existing = inFlight[source.cacheKey] {
			return await existing.value
		}
		let task = Task<UIImage?, Never> { [weak self] in
			await self?.fetch(source) ?? nil
		}
		inFlight[source.cacheKey] = task
		let image = await task.value
		inFlight[source.cacheKey] = nil
		return image
	}

	private func fetch(_ source: CoverSource) async -> UIImage? {
		let entry = Entry(directory: directory, key: source.cacheKey)
		let stored = entry.read()

		var request = URLRequest(url: source.url)
		// gaindrive sends `Cache-Control: no-cache` plus an `ETag` over the
		// file's mtime, size and the requested dimensions — deliberately, since
		// `folders.id` is a rowid that changes across a rescan and a blindly
		// cached cover would show the previous album's art after a rebuild.
		// Revalidating rather than trusting the stored copy is what keeps the
		// stable key honest.
		if let tag = stored?.etag {
			request.setValue(tag, forHTTPHeaderField: "If-None-Match")
		}

		guard let (data, response) = try? await session.data(for: request),
			let http = response as? HTTPURLResponse
		else {
			return stored.flatMap { UIImage(data: $0.data) }
		}

		if http.statusCode == 304, let stored {
			return remember(UIImage(data: stored.data), for: source.cacheKey)
		}
		guard (200..<300).contains(http.statusCode), let image = UIImage(data: data) else {
			// 404 is the ordinary answer for an artist with no portrait, so it
			// is not worth logging or retrying — the caller draws a placeholder.
			return nil
		}
		entry.write(data: data, etag: http.value(forHTTPHeaderField: "ETag"))
		trimIfNeeded()
		return remember(image, for: source.cacheKey)
	}

	private func remember(_ image: UIImage?, for key: String) -> UIImage? {
		guard let image else { return nil }
		memory.setObject(image, forKey: key as NSString)
		return image
	}

	/// One file per image plus its `ETag` in an extended attribute would be
	/// tidier, but extended attributes do not survive every filesystem the
	/// caches directory can live on. Two files it is.
	private struct Entry {
		let directory: URL
		let key: String

		private var name: String {
			// The key contains `/` and `#`, neither of which can be in a file
			// name, and a hash also bounds the length — a server UUID plus a
			// long album id does not.
			//
			// **SHA-256 rather than `hashValue`.** Swift seeds `Hasher` per
			// process, so `hashValue` gives a different answer on every launch
			// — which would miss the entire disk cache exactly when it is
			// supposed to hit, and this cache exists for no other reason.
			SHA256.hash(data: Data(key.utf8))
				.prefix(16)
				.map { String(format: "%02x", $0) }
				.joined()
		}

		var dataURL: URL { directory.appending(path: name) }
		var etagURL: URL { directory.appending(path: name + ".etag") }

		func read() -> (data: Data, etag: String?)? {
			guard let data = try? Data(contentsOf: dataURL) else { return nil }
			let etag = try? String(contentsOf: etagURL, encoding: .utf8)
			return (data, etag)
		}

		func write(data: Data, etag: String?) {
			try? data.write(to: dataURL, options: .atomic)
			if let etag {
				try? etag.write(to: etagURL, atomically: true, encoding: .utf8)
			}
		}
	}

	/// Evicts oldest-first when the directory outgrows its cap.
	///
	/// By modification date rather than access date: `relatime` and its
	/// equivalents make access times unreliable, which is the same reason the
	/// server's own transcode cache evicts on mtime.
	private func trimIfNeeded() {
		let keys: [URLResourceKey] = [.contentModificationDateKey, .fileSizeKey]
		guard
			let files = try? FileManager.default.contentsOfDirectory(
				at: directory, includingPropertiesForKeys: keys)
		else { return }

		var total = 0
		var entries: [(url: URL, date: Date, size: Int)] = []
		for file in files {
			guard let values = try? file.resourceValues(forKeys: Set(keys)),
				let size = values.fileSize
			else { continue }
			total += size
			entries.append((file, values.contentModificationDate ?? .distantPast, size))
		}
		guard total > Self.diskLimit else { return }

		// Down to 0.9× the cap rather than exactly to it, so a steady stream of
		// requests does not trigger a delete on every single one.
		let target = Int(Double(Self.diskLimit) * 0.9)
		for entry in entries.sorted(by: { $0.date < $1.date }) {
			guard total > target else { break }
			try? FileManager.default.removeItem(at: entry.url)
			total -= entry.size
		}
	}
}
