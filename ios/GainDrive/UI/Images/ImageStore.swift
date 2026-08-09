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
/// **The actor owns bookkeeping only.** The disk read, the decode and the write
/// happen in `load`, a `static async` function outside the actor's isolation,
/// so they run on the cooperative pool. They used to be isolated methods, which made the disk
/// concurrency exactly one: fifty rows meant fifty whole-file reads and decodes
/// back to back on a single thread, and covers appeared in a visible ripple
/// rather than together.
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

	/// Keys the server has answered 404 for.
	///
	/// Not every id has art — an album folder with no cover file, an artist
	/// with no portrait — and without remembering that, every miss is re-asked
	/// on every re-appearance, every scroll pass and every screen visit. In
	/// memory only, deliberately: art can be added to a folder, and a
	/// relaunch is a reasonable moment to look again.
	private var missing: Set<String> = []

	/// The cache's size on disk, measured once and then tracked.
	///
	/// It used to be recomputed by walking the whole directory after *every*
	/// download — two files per image, so a few hundred covers meant ~600 stats
	/// per download, and at thumbnail sizes the cap needs thousands of files to
	/// trip, so essentially all of that work was thrown away every time.
	private var diskBytes: Int?

	// The gate. `httpMaximumConnectionsPerHost` is 6, and
	// `timeoutIntervalForRequest` counts **while a request waits for a free
	// connection** — so handing `URLSession` a screenful of covers at once made
	// the tail time out having never been sent, and the row then showed the
	// placeholder for ever because nothing retries. Four also leaves slots for
	// the API calls, which share the session and were being starved by artwork.
	private var active = 0
	private var waiting: [CheckedContinuation<Void, Never>] = []
	private static let maxConcurrent = 4

	private static let diskLimit = 128 << 20

	init(session: URLSession = HTTP.shared) {
		self.session = session
		let caches = FileManager.default.urls(for: .cachesDirectory, in: .userDomainMask)[0]
		directory = caches.appending(path: "covers", directoryHint: .isDirectory)
		try? FileManager.default.createDirectory(at: directory, withIntermediateDirectories: true)
		memory.countLimit = 400
		// Bytes as well as count: 400 hero images at 800 px is a great deal
		// more memory than 400 thumbnails, and only one of those numbers
		// notices.
		memory.totalCostLimit = 64 << 20
	}

	func image(for source: CoverSource) async -> UIImage? {
		let key = source.cacheKey
		if let cached = memory.object(forKey: key as NSString) { return cached }
		if missing.contains(key) { return nil }
		if let existing = inFlight[key] { return await existing.value }

		// Registered before any suspension point, so two callers cannot both
		// miss and both start a fetch. The gate is taken *inside* the task for
		// the same reason: waiting for a slot out here would suspend between
		// the check above and the insert below.
		let task = Task<UIImage?, Never> { [session, directory] in
			await self.acquire()
			let fetched = await Self.load(source, session: session, directory: directory)
			await self.finish(fetched, for: key)
			return fetched.image
		}
		inFlight[key] = task
		let image = await task.value
		inFlight[key] = nil
		return image
	}

	// MARK: - Bookkeeping

	private struct Fetched: Sendable {
		let image: UIImage?
		let bytesWritten: Int
		/// The server said there is no such image, as opposed to failing to
		/// answer. Only the first is worth remembering.
		let isMissing: Bool
	}

	private func finish(_ fetched: Fetched, for key: String) {
		release()
		if let image = fetched.image {
			// The decoded footprint, not the transferred bytes — those are zero
			// on the 304 and disk paths, which would let the cache fill with
			// images it believes are free.
			let pixels = image.size.width * image.size.height * image.scale * image.scale
			memory.setObject(image, forKey: key as NSString, cost: Int(pixels) * 4)
		}
		if fetched.isMissing { missing.insert(key) }
		guard fetched.bytesWritten > 0 else { return }

		// Measured once, on the first write of the session, and tracked from
		// there. The walk when the cap is genuinely exceeded is rare enough to
		// be worth doing here rather than building more machinery around.
		let total = (diskBytes ?? Self.measure(directory)) + fetched.bytesWritten
		guard total > Self.diskLimit else {
			diskBytes = total
			return
		}
		diskBytes = Self.trim(directory, to: Int(Double(Self.diskLimit) * 0.9))
	}

	private func acquire() async {
		guard active >= Self.maxConcurrent else {
			active += 1
			return
		}
		await withCheckedContinuation { waiting.append($0) }
		// Resumed by `release`, which hands the slot over rather than freeing
		// it — so `active` is already correct and must not be incremented here.
	}

	private func release() {
		guard !waiting.isEmpty else {
			active -= 1
			return
		}
		waiting.removeFirst().resume()
	}

	// MARK: - Off the actor

	/// Read, fetch, write.
	///
	/// `static`, so it is outside the actor's isolation, and `async`, so
	/// awaiting it from an isolated context hops to the cooperative pool. That
	/// pair is what keeps the disk read, the decode and the write off the
	/// actor's serial executor — where they used to make disk concurrency
	/// exactly one.
	private static func load(
		_ source: CoverSource, session: URLSession, directory: URL
	) async -> Fetched {
		let entry = Entry(directory: directory, key: source.cacheKey)
		let stored = entry.read()

		var request = URLRequest(url: source.url)
		// **Our `ETag` is the authoritative one.** Left on the default policy,
		// `URLCache` holds its own entry for the same URL and can satisfy or
		// revalidate the request itself, handing back a synthesised 200 — so
		// the 304 branch below never runs and the body is re-decoded anyway.
		// Nothing is lost by opting out: the server sends `Cache-Control:
		// no-cache` and the URL carries a per-launch salt, so `URLCache` could
		// never hit for a cover in the first place.
		request.cachePolicy = .reloadIgnoringLocalCacheData
		// gaindrive sends an `ETag` over the file's mtime, size and the
		// requested dimensions — deliberately, since `folders.id` is a rowid
		// that changes across a rescan and a blindly cached cover would show
		// the previous album's art after a rebuild. Revalidating rather than
		// trusting the stored copy is what keeps the stable key honest.
		if let tag = stored?.etag {
			request.setValue(tag, forHTTPHeaderField: "If-None-Match")
		}

		guard let (data, response) = try? await session.data(for: request),
			let http = response as? HTTPURLResponse
		else {
			// Unreachable, not absent. Show what is on disk and do not record a
			// miss — the next attempt may well succeed.
			return Fetched(
				image: stored.flatMap { UIImage(data: $0.data) }, bytesWritten: 0,
				isMissing: false)
		}

		if http.statusCode == 304, let stored {
			return Fetched(image: UIImage(data: stored.data), bytesWritten: 0, isMissing: false)
		}
		guard (200..<300).contains(http.statusCode), let image = UIImage(data: data) else {
			// 404 is the ordinary answer for a folder with no artwork, so it is
			// not worth logging or retrying — the caller draws a placeholder.
			return Fetched(image: nil, bytesWritten: 0, isMissing: http.statusCode == 404)
		}
		entry.write(data: data, etag: http.value(forHTTPHeaderField: "ETag"))
		return Fetched(image: image, bytesWritten: data.count, isMissing: false)
	}

	/// One file per image plus its `ETag` beside it. An extended attribute
	/// would be tidier, but extended attributes do not survive every filesystem
	/// the caches directory can live on.
	private struct Entry {
		let directory: URL
		let key: String

		static let etagSuffix = ".etag"

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
		var etagURL: URL { directory.appending(path: name + Self.etagSuffix) }

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

	private static func measure(_ directory: URL) -> Int {
		let files =
			(try? FileManager.default.contentsOfDirectory(
				at: directory, includingPropertiesForKeys: [.fileSizeKey])) ?? []
		return files.reduce(0) { $0 + ((try? $1.resourceValues(forKeys: [.fileSizeKey]))?.fileSize ?? 0) }
	}

	/// Evicts oldest-first until the directory is under `target`, and returns
	/// the new total.
	///
	/// Only the data files are ranked, and each one's `ETag` file goes with it.
	/// Treating the two kinds as independent entries — which an earlier version
	/// did — can delete an `ETag` while keeping its data, after which every
	/// request for that cover re-downloads the whole body instead of
	/// revalidating.
	///
	/// By modification date rather than access date: `relatime` and its
	/// equivalents make access times unreliable, which is the same reason the
	/// server's own transcode cache evicts on mtime.
	private static func trim(_ directory: URL, to target: Int) -> Int {
		let keys: [URLResourceKey] = [.contentModificationDateKey, .fileSizeKey]
		guard
			let files = try? FileManager.default.contentsOfDirectory(
				at: directory, includingPropertiesForKeys: keys)
		else { return 0 }

		var total = 0
		var entries: [(data: URL, date: Date, size: Int)] = []
		var etagSizes: [String: Int] = [:]

		for file in files {
			let size = (try? file.resourceValues(forKeys: [.fileSizeKey]))?.fileSize ?? 0
			total += size
			if file.lastPathComponent.hasSuffix(Entry.etagSuffix) {
				etagSizes[String(file.lastPathComponent.dropLast(Entry.etagSuffix.count))] = size
				continue
			}
			let date =
				(try? file.resourceValues(forKeys: keys))?.contentModificationDate ?? .distantPast
			entries.append((file, date, size))
		}

		for entry in entries.sorted(by: { $0.date < $1.date }) {
			guard total > target else { break }
			try? FileManager.default.removeItem(at: entry.data)
			total -= entry.size
			let etag = entry.data.appendingPathExtension(String(Entry.etagSuffix.dropFirst()))
			try? FileManager.default.removeItem(at: etag)
			total -= etagSizes[entry.data.lastPathComponent] ?? 0
		}
		return total
	}
}
