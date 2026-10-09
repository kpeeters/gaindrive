//	GainDrive for iOS
//	Copyright (C) 2026 Kasper Peeters
//
//	This file is part of GainDrive, distributed under the GNU General
//	Public License version 3 or later, with the Application Distribution
//	Exception in ios/LICENSE. See LICENSE at the repository root for the
//	full text of the GPL.

import AVFoundation
import os

/// Pinned films, fetched as HLS by the system's asset downloader.
///
/// **HLS always, never the file**, even for a film the player would stream
/// as one. A film the device cannot decode would download whole and then not
/// play; the server's playlist is H.264 in every case, and one transport
/// means one kind of stored film.
///
/// **The system's downloader rather than our own segment loop.** A background
/// `URLSession` runs one long transfer well and a thousand short ones badly:
/// the system throttles a suspended app's task creation, so a film fetched a
/// segment at a time would crawl whenever the app is not open. An asset
/// download runs out of process, across suspension and relaunch, like
/// `DownloadQueue`'s tasks do.
///
/// The cache key travels in `taskDescription`, as in `DownloadQueue`, because
/// it is the only state that survives a relaunch.
final class FilmQueue: NSObject, @unchecked Sendable {
	var onProgress: (@MainActor (ItemRef, Double) -> Void)?
	var onFinished: (@MainActor (ItemRef) -> Void)?
	var onFailed: (@MainActor (ItemRef) -> Void)?
	/// See `DownloadQueue.backgroundCompletion`.
	var backgroundCompletion: (@MainActor () -> Void)?

	private let store: FilmStore
	/// Final locations between `didFinishDownloadingTo` and completion.
	/// Touched only from delegate callbacks, which the session delivers on
	/// one serial queue.
	private var finished: [ItemRef: URL] = [:]
	/// Assigned once in `init`, for the reason `DownloadQueue.session` is.
	private var session: AVAssetDownloadURLSession!

	static let identifier = "org.gaindrive.ios-player.films"
	private static let log = Logger(subsystem: "org.gaindrive.ios-player", category: "films")

	init(store: FilmStore) {
		self.store = store
		super.init()
		let config = URLSessionConfiguration.background(withIdentifier: Self.identifier)
		config.isDiscretionary = false
		config.sessionSendsLaunchEvents = true
		session = AVAssetDownloadURLSession(
			configuration: config, assetDownloadDelegate: self, delegateQueue: nil)
	}

	/// See `DownloadQueue.resume`.
	func resume() {
		session.getAllTasks { _ in }
	}

	/// No variant is chosen, so the task takes the highest the master
	/// playlist offers.
	func start(_ ref: ItemRef, title: String, url: URL, allowsExpensive: Bool) {
		// "Wi-Fi only" is an asset option here: the request belongs to
		// AVFoundation, not to us.
		let asset = AVURLAsset(
			url: url, options: [AVURLAssetAllowsExpensiveNetworkAccessKey: allowsExpensive])
		let configuration = AVAssetDownloadConfiguration(asset: asset, title: title)
		let task = session.makeAssetDownloadTask(downloadConfiguration: configuration)
		task.taskDescription = Self.key(ref)
		task.resume()
	}

	func cancel(_ ref: ItemRef) {
		let key = Self.key(ref)
		session.getAllTasks { tasks in
			for task in tasks where task.taskDescription == key { task.cancel() }
		}
	}

	func cancelAll() {
		session.getAllTasks { $0.forEach { $0.cancel() } }
	}

	func cancelAllNow() async {
		for task in await session.allTasks { task.cancel() }
	}

	/// What is in flight. The fraction is not kept by the task, so a relaunch
	/// shows zero until the next progress report.
	func inFlight() async -> [ItemRef: Double] {
		var found: [ItemRef: Double] = [:]
		for task in await session.allTasks {
			if let ref = Self.ref(task) { found[ref] = 0 }
		}
		return found
	}

	private static func key(_ ref: ItemRef) -> String {
		CacheKeys.of(ref, quality: .original)
	}

	private static func ref(_ task: URLSessionTask) -> ItemRef? {
		task.taskDescription.flatMap(CacheKeys.parse)?.ref
	}

	private func report(_ body: @escaping @MainActor () -> Void) {
		Task { @MainActor in body() }
	}
}

extension FilmQueue: AVAssetDownloadDelegate {
	/// Recorded as soon as it is known, so removing a film mid-download can
	/// delete what has arrived so far.
	func urlSession(
		_ session: URLSession, assetDownloadTask: AVAssetDownloadTask,
		willDownloadTo location: URL
	) {
		guard let ref = Self.ref(assetDownloadTask) else { return }
		let store = store
		Task { await store.begin(ref, at: location) }
	}

	/// Arrives before the task completes, and is not yet success: that is
	/// `didCompleteWithError` with no error.
	func urlSession(
		_ session: URLSession, assetDownloadTask: AVAssetDownloadTask,
		didFinishDownloadingTo location: URL
	) {
		guard let ref = Self.ref(assetDownloadTask) else { return }
		finished[ref] = location
	}

	func urlSession(
		_ session: URLSession, assetDownloadTask: AVAssetDownloadTask,
		didLoad timeRange: CMTimeRange, totalTimeRangesLoaded loadedTimeRanges: [NSValue],
		timeRangeExpectedToLoad: CMTimeRange
	) {
		guard let ref = Self.ref(assetDownloadTask) else { return }
		let expected = timeRangeExpectedToLoad.duration.seconds
		guard expected > 0 else { return }
		let loaded = loadedTimeRanges.reduce(0.0) { $0 + $1.timeRangeValue.duration.seconds }
		let fraction = min(loaded / expected, 1)
		report { self.onProgress?(ref, fraction) }
	}

	func urlSession(
		_ session: URLSession, task: URLSessionTask, didCompleteWithError error: (any Error)?
	) {
		guard let ref = Self.ref(task) else { return }
		let store = store
		let location = finished.removeValue(forKey: ref)
		if error == nil {
			Task {
				// One task, so the final location is recorded before the
				// package is called whole.
				if let location { await store.begin(ref, at: location) }
				await store.complete(ref)
				#if os(iOS)
					// Last to go when the device runs out of room, rather
					// than among the first as a re-fetchable asset would be.
					if let location = await store.location(for: ref) {
						let policy = AVMutableAssetDownloadStorageManagementPolicy()
						policy.priority = .important
						policy.expirationDate = .distantFuture
						AVAssetDownloadStorageManager.shared().setStorageManagementPolicy(
							policy, for: location)
					}
				#endif
				report { self.onFinished?(ref) }
			}
			return
		}
		// The session hands over the partial package even for a task that did
		// not finish, and it is ours to delete.
		if let location { try? FileManager.default.removeItem(at: location) }
		// A cancellation is the user unpinning, or a restart under a new
		// network policy, whose new package must not be discarded here.
		guard (error as? URLError)?.code != .cancelled else { return }
		Self.log.error(
			"download of \(ref.encoded, privacy: .public) failed: \(String(describing: error), privacy: .public)")
		Task { await store.discard(ref) }
		report { self.onFailed?(ref) }
	}

	func urlSessionDidFinishEvents(forBackgroundURLSession session: URLSession) {
		report { self.backgroundCompletion?() }
	}
}
