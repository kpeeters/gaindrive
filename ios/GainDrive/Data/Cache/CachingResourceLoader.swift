//	GainDrive for iOS
//	Copyright (C) 2026 Kasper Peeters
//
//	This file is part of GainDrive, distributed under the GNU General
//	Public License version 3 or later, with the Application Distribution
//	Exception in ios/LICENSE. See LICENSE at the repository root for the
//	full text of the GPL.

import AVFoundation
import Foundation
import UniformTypeIdentifiers

/// Plays a track from the copy it is downloading.
///
/// **One task, one growing file, one integer.** The track is fetched once, in
/// order, at whatever speed the network gives - not at playback rate - and the
/// player reads out of the file as it fills. When the fetch completes the file
/// is adopted by `AudioStore` under the same key a pinned download uses, so a
/// track cached by playing it satisfies a later pin without re-fetching.
///
/// It exists because **`AVURLAsset` will not play a file that is still
/// growing**: it reads a file URL as a complete asset and does not wait. The
/// resource loader is the only thing on this platform that can hold a read
/// until the bytes arrive.
///
/// Filling sequentially is what makes it tractable. A general byte cache has to
/// track which ranges it holds and fetch the gaps; here what is present is
/// always a prefix, so the bookkeeping is `PartialFile.received` and nothing
/// else.
///
/// **Everything is confined to one serial queue.** AVFoundation is given it as
/// its delegate queue and `URLSession` is given an `OperationQueue` backed by
/// it, so the two sets of callbacks cannot interleave and none of this needs
/// locking. That is also why the class is `@unchecked Sendable` rather than an
/// actor: an `AVAssetResourceLoadingRequest` is not `Sendable` and cannot cross
/// an actor boundary at all.
final class CachingResourceLoader: NSObject, @unchecked Sendable {
	/// AVFoundation consults a delegate **only** for schemes it does not handle
	/// itself, so a playback URL is rewritten to this and the real one kept
	/// here. Anything `http` or `https` it fetches on its own and the delegate
	/// is never called.
	static let scheme = "gaindrive-cache"

	static func rewrite(_ url: URL) -> URL {
		var components = URLComponents(url: url, resolvingAgainstBaseURL: false)
		components?.scheme = scheme
		return components?.url ?? url
	}

	let queue = DispatchQueue(label: "org.gaindrive.ios.resource-loader")

	private let source: URL
	private let ref: ItemRef
	private let quality: AudioQuality
	private let store: AudioStore

	private var session: URLSession!
	private var task: URLSessionDataTask?
	private var part: PartialFile?
	private var pending: [AVAssetResourceLoadingRequest] = []
	private var contentLength: Int?
	private var contentUTI: String?
	private var started = false
	/// The fetch finished and the file was renamed into place. The handles stay
	/// open - see `didCompleteWithError`.
	private var adopted = false
	/// Set when this resource cannot be cached, after which every request is
	/// handed back to AVFoundation to fetch for itself. See `giveUp`.
	private var redirecting = false

	init(source: URL, ref: ItemRef, quality: AudioQuality, store: AudioStore) {
		self.source = source
		self.ref = ref
		self.quality = quality
		self.store = store
		super.init()

		let operations = OperationQueue()
		operations.maxConcurrentOperationCount = 1
		operations.underlyingQueue = queue
		let config = URLSessionConfiguration.default
		// The point of this class is to fetch faster than playback, so nothing
		// here is discretionary and nothing is cached twice: the part file is
		// the cache.
		config.urlCache = nil
		config.requestCachePolicy = .reloadIgnoringLocalCacheData
		// The server materialises a transcode in full before sending a byte, so
		// a long track can legitimately produce nothing for a while.
		config.timeoutIntervalForRequest = 300
		session = URLSession(configuration: config, delegate: self, delegateQueue: operations)
	}

	/// **Cancels the fetch.** The loader runs ahead of playback by design, so
	/// without this a track skipped after five seconds would still be pulled
	/// down in full. The asset holds the delegate, the window holds the asset,
	/// and dropping a window entry is what gets here.
	deinit {
		task?.cancel()
		// Discarded only if it never completed. After adoption the same file is
		// a stored track, and deleting it here would throw away what was just
		// cached the moment the track stopped playing.
		if adopted { part?.close() } else { part?.discard() }
	}
}

// MARK: - AVFoundation

extension CachingResourceLoader: AVAssetResourceLoaderDelegate {
	func resourceLoader(
		_ resourceLoader: AVAssetResourceLoader,
		shouldWaitForLoadingOfRequestedResource request: AVAssetResourceLoadingRequest
	) -> Bool {
		startIfNeeded()
		pending.append(request)
		serve()
		return true
	}

	func resourceLoader(
		_ resourceLoader: AVAssetResourceLoader, didCancel request: AVAssetResourceLoadingRequest
	) {
		pending.removeAll { $0 === request }
	}

	private func startIfNeeded() {
		guard !started else { return }
		started = true
		part = PartialFile(url: store.partURL(for: ref, quality: quality))
		var request = URLRequest(url: source)
		request.setValue("identity", forHTTPHeaderField: "Accept-Encoding")
		task = session.dataTask(with: request)
		task?.resume()
	}

	/// Answers everything it can and leaves the rest pending.
	private func serve() {
		guard !redirecting else {
			serveRedirects()
			return
		}
		guard let part else { return }
		var stillPending: [AVAssetResourceLoadingRequest] = []

		for request in pending {
			guard !request.isCancelled else { continue }

			if let info = request.contentInformationRequest {
				guard let length = contentLength else {
					stillPending.append(request)
					continue
				}
				// **A UTI, not a MIME type.** AVFoundation accepts a MIME type
				// here without complaint and then plays nothing, which is the
				// same silent-stall failure a file stored without an extension
				// produces. There is no error to catch either way.
				info.contentType = contentUTI
				info.contentLength = Int64(length)
				info.isByteRangeAccessSupported = true
			}

			guard let data = request.dataRequest else {
				request.finishLoading()
				continue
			}

			let end = PartialRead.end(
				requestedOffset: Int(data.requestedOffset),
				requestedLength: data.requestedLength,
				toEnd: data.requestsAllDataToEndOfResource,
				total: contentLength)

			if let chunk = PartialRead.chunk(
				currentOffset: Int(data.currentOffset), end: end, received: part.received),
				let bytes = part.read(chunk)
			{
				data.respond(with: bytes)
			}

			if PartialRead.isSatisfied(currentOffset: Int(data.currentOffset), end: end) {
				request.finishLoading()
			} else {
				// Held, and retried on the next chunk. A read past what has
				// arrived waits - which for a fetch running at network speed is
				// a moment, and is the one place a slow link is felt.
				stillPending.append(request)
			}
		}
		pending = stillPending
	}

	/// Stops caching this resource and lets AVFoundation fetch it directly.
	///
	/// Safe at this point precisely because nothing has been served yet: no
	/// data request can have been answered while the length was unknown, so a
	/// redirect is still open to us.
	private func giveUp() {
		redirecting = true
		task?.cancel()
		task = nil
		part?.discard()
		part = nil
		serveRedirects()
	}

	/// **Handed back rather than failed.** AVFoundation follows a redirect out
	/// of our scheme and fetches the real URL itself, which is exactly the
	/// uncached streaming this app did before the cache existed. Failing
	/// instead would turn a server that cannot produce a length into a track
	/// that will not play at all.
	private func serveRedirects() {
		for request in pending where !request.isCancelled {
			request.redirect = URLRequest(url: source)
			request.response = HTTPURLResponse(
				url: source, statusCode: 302, httpVersion: nil, headerFields: nil)
			request.finishLoading()
		}
		pending = []
	}

	private func failAll(_ error: any Error) {
		for request in pending where !request.isCancelled {
			request.finishLoading(with: error)
		}
		pending = []
	}
}

// MARK: - Fetching

extension CachingResourceLoader: URLSessionDataDelegate {
	func urlSession(
		_ session: URLSession, dataTask: URLSessionDataTask, didReceive response: URLResponse,
		completionHandler: @escaping (URLSession.ResponseDisposition) -> Void
	) {
		defer { completionHandler(.allow) }
		guard let http = response as? HTTPURLResponse,
			(200..<300).contains(http.statusCode), response.expectedContentLength > 0
		else {
			// **No length, no cache** - and that is a real case, not a
			// defensive one: the server falls back to a piped transcode when
			// its cache cannot produce a file (a full disk, a busy job queue,
			// an ffmpeg error), and a piped response is chunked with no
			// `Content-Length`. A content information request cannot be
			// answered without one, and the player would wait for ever.
			//
			// So the resource is given back instead of failed, and playback is
			// what it was before this class existed.
			giveUp()
			return
		}
		contentLength = Int(response.expectedContentLength)
		contentUTI = Self.uti(for: response, quality: quality)
		serve()
	}

	func urlSession(_ session: URLSession, dataTask: URLSessionDataTask, didReceive data: Data) {
		part?.append(data)
		serve()
	}

	func urlSession(
		_ session: URLSession, task: URLSessionTask, didCompleteWithError error: (any Error)?
	) {
		guard !redirecting else { return }
		guard let part else { return }

		guard error == nil, let length = contentLength, part.received >= length else {
			// **Nothing incomplete is ever adopted.** A cancelled or failed
			// fetch leaves a part file, and half a track that claims to be
			// whole is worse than no track at all.
			self.part = nil
			part.discard()
			if let error { failAll(error) }
			return
		}

		// **Answer everything before the file moves, and keep answering
		// after.** This is the step whose absence hung playback: the server
		// writes MP4 with its index at the end, so AVFoundation's early read is
		// for the *tail* - a request that is necessarily still pending when the
		// last byte arrives. Dropping the file here left it unanswered for
		// ever, and the spinner never stopped.
		serve()
		adopted = true
		try? store.adopt(
			part.url, for: ref, quality: quality,
			fileExtension: DownloadQueue.fileExtension(for: task.response, quality: quality))
		// **The handles stay open across the rename**, which POSIX allows: a
		// descriptor follows the inode, not the name. So a seek later in the
		// same playback is still served from here rather than finding nothing.
		Task { [store] in await store.finishedAdopting() }
	}

	/// The type AVFoundation is told the bytes are.
	///
	/// The format usually settles it; the original's container is whatever the
	/// server holds, so the response's MIME type is asked. Both paths end in a
	/// UTI, which is what the content information request wants - see `serve`.
	static func uti(for response: URLResponse?, quality: AudioQuality) -> String? {
		// **The response first, the quality only as a fallback.** The order
		// used to be reversed, which was right while the format settled what
		// arrived. It no longer does: a request that declared what it takes as
		// it stands - see `PlayableAudio` - may be answered with the file the
		// server holds rather than the format asked for, and telling
		// AVFoundation the wrong one plays nothing, silently, exactly as the
		// note on the content-information request above warns.
		if let mime = response?.mimeType, let type = UTType(mimeType: mime) {
			return type.identifier
		}
		guard let declared = quality.format.contentType,
			let type = UTType(mimeType: declared)
		else { return nil }
		return type.identifier
	}
}
