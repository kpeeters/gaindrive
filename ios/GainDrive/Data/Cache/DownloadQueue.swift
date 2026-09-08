//	GainDrive for iOS
//	Copyright (C) 2026 Kasper Peeters
//
//	This file is part of GainDrive, distributed under the GNU General
//	Public License version 3 or later, with the Application Distribution
//	Exception in ios/LICENSE. See LICENSE at the repository root for the
//	full text of the GPL.

import Foundation
import UniformTypeIdentifiers

/// One track's download, as the UI sees it.
enum DownloadState: Hashable, Sendable {
	case absent
	case running(fraction: Double)
	/// Here because it was played, and evictable tonight.
	case cached
	/// Here because it was asked for, and safe from eviction.
	case stored
	case failed

	var isHere: Bool {
		self == .cached || self == .stored
	}
}

/// Fetches pinned tracks with a **background** `URLSession`.
///
/// Background rather than default, and that is the one place this beats
/// Android: the system keeps the transfers going while the app is suspended and
/// resumes them across launches and reboots, where Android gave up by
/// installing no `Scheduler`. Pinning an album is minutes of downloading, so
/// "only while you are looking at it" is not a feature.
///
/// It costs one thing the app did not have: an `AppDelegate` to hold the
/// completion handler the system hands over when it wakes the app to finish.
/// See `AppDelegate`.
///
/// Two consequences of being a background session are worth knowing before
/// changing anything here:
///
/// * **The delegate is the only way results arrive.** There is no `await` form,
///   and callbacks can land in a process that has just launched with no memory
///   of asking — which is why the cache key travels in `taskDescription` rather
///   than in a map. It is the only state that survives.
/// * **A task may sit with no bytes for a long time and be perfectly healthy.**
///   The server materialises a transcode in full before sending any of it, so
///   the resource timeout is what matters and the per-request one would only
///   cancel work that was going to succeed. Both are set accordingly.
final class DownloadQueue: NSObject, @unchecked Sendable {
	/// Called on the main actor as tasks progress. Set once, by
	/// `PinRepository`.
	var onProgress: (@MainActor (ItemRef, Double) -> Void)?
	var onFinished: (@MainActor (ItemRef) -> Void)?
	var onFailed: (@MainActor (ItemRef) -> Void)?

	/// Set by `AppDelegate` when the system wakes us to finish, and called back
	/// once the session says it has nothing left to report. Skipping it makes
	/// the system consider the app unresponsive and stop waking it.
	var backgroundCompletion: (@MainActor () -> Void)?

	private let store: AudioStore
	/// Implicitly unwrapped, and assigned once, because the session needs
	/// `self` as its delegate and `self` does not exist until `super.init()`
	/// has run. `lazy` would do the same job and would be initialised on
	/// whichever thread touched it first, which for a type whose callbacks
	/// arrive off the main actor is a race waiting to be written.
	private var session: URLSession!

	private static let identifier = "org.gaindrive.ios.downloads"

	init(store: AudioStore) {
		self.store = store
		super.init()
		let config = URLSessionConfiguration.background(withIdentifier: Self.identifier)
		// A download somebody asked for should not be waiting for the system to
		// decide it is a good moment.
		config.isDiscretionary = false
		config.sessionSendsLaunchEvents = true
		// Deliberately generous: see the note above about the server building a
		// transcode in full before it sends a byte.
		config.timeoutIntervalForRequest = 300
		config.timeoutIntervalForResource = 24 * 60 * 60
		session = URLSession(configuration: config, delegate: self, delegateQueue: nil)
	}

	/// Starts the session so a relaunch re-adopts whatever was in flight.
	/// Without this the tasks are still running but nothing is listening, and
	/// their results arrive only when something else happens to touch the
	/// session.
	func resume() {
		session.getAllTasks { _ in }
	}

	func start(_ ref: ItemRef, quality: AudioQuality, url: URL) {
		let task = session.downloadTask(with: url)
		// The one piece of state that outlives the process.
		task.taskDescription = CacheKeys.of(ref, quality: quality)
		task.resume()
	}

	func cancel(_ ref: ItemRef) {
		session.getAllTasks { tasks in
			for task in tasks {
				guard let key = task.taskDescription, let parsed = CacheKeys.parse(key) else {
					continue
				}
				if parsed.ref == ref { task.cancel() }
			}
		}
	}

	func cancelAll() {
		session.getAllTasks { $0.forEach { $0.cancel() } }
	}

	/// What is in flight, so a relaunch can show progress rather than "absent"
	/// for tracks the system is still fetching.
	func inFlight() async -> [ItemRef: Double] {
		let tasks = await session.allTasks
		var found: [ItemRef: Double] = [:]
		for task in tasks {
			guard let key = task.taskDescription, let parsed = CacheKeys.parse(key) else {
				continue
			}
			let total = task.countOfBytesExpectedToReceive
			found[parsed.ref] = total > 0 ? Double(task.countOfBytesReceived) / Double(total) : 0
		}
		return found
	}
}

extension DownloadQueue: URLSessionDownloadDelegate {
	/// **The file must be moved before this returns.** `URLSession` deletes the
	/// temporary file the moment the delegate comes back, so awaiting the store
	/// first would reliably lose every download. `AudioStore.adopt` is
	/// `nonisolated` for exactly this call.
	func urlSession(
		_ session: URLSession, downloadTask: URLSessionDownloadTask,
		didFinishDownloadingTo location: URL
	) {
		guard let key = downloadTask.taskDescription, let parsed = CacheKeys.parse(key) else {
			return
		}
		guard
			let response = downloadTask.response as? HTTPURLResponse,
			(200..<300).contains(response.statusCode)
		else {
			// A Subsonic error is a 200 carrying a failure envelope, so this
			// catches only transport-level refusals — the rest is caught by
			// what lands on disk being unplayable, which is a stage 2 concern.
			report { self.onFailed?(parsed.ref) }
			return
		}

		do {
			try store.adopt(
				location, for: parsed.ref, quality: parsed.quality,
				fileExtension: Self.fileExtension(
					for: downloadTask.response, quality: parsed.quality))
		} catch {
			report { self.onFailed?(parsed.ref) }
			return
		}
		let store = store
		Task { await store.finishedAdopting() }
		report { self.onFinished?(parsed.ref) }
	}

	func urlSession(
		_ session: URLSession, downloadTask: URLSessionDownloadTask, didWriteData: Int64,
		totalBytesWritten: Int64, totalBytesExpectedToWrite: Int64
	) {
		guard let key = downloadTask.taskDescription, let parsed = CacheKeys.parse(key) else {
			return
		}
		// `URLSessionTaskDelegate` reports continuous byte progress, which is
		// the second thing this beats Android at — Media3 could only report
		// whole tracks.
		let fraction =
			totalBytesExpectedToWrite > 0
			? Double(totalBytesWritten) / Double(totalBytesExpectedToWrite) : 0
		report { self.onProgress?(parsed.ref, fraction) }
	}

	func urlSession(
		_ session: URLSession, task: URLSessionTask, didCompleteWithError error: (any Error)?
	) {
		guard error != nil else { return }
		guard let key = task.taskDescription, let parsed = CacheKeys.parse(key) else { return }
		// A cancellation is the user unpinning, not a failure to report.
		guard (error as? URLError)?.code != .cancelled else { return }
		report { self.onFailed?(parsed.ref) }
	}

	func urlSessionDidFinishEvents(forBackgroundURLSession session: URLSession) {
		report { self.backgroundCompletion?() }
	}

	/// What to name the stored file.
	///
	/// AVFoundation types a local file by its path extension, so this is not
	/// cosmetic: a file without one is never reported as unplayable, the player
	/// simply waits, and the symptom is a track that never starts.
	///
	/// The format usually says. The original does not — its container is
	/// whatever the server holds — so the response is asked instead, which is
	/// the one moment that answer is available.
	static func fileExtension(for response: URLResponse?, quality: AudioQuality) -> String {
		if let known = quality.format.fileExtension { return known }
		if let mime = response?.mimeType, let type = UTType(mimeType: mime),
			let derived = type.preferredFilenameExtension
		{
			return derived
		}
		let suggested = (response?.suggestedFilename as NSString?)?.pathExtension ?? ""
		// `audio` is not a container anything can read, and that is the point:
		// it is better to store a file AVFoundation refuses outright than one
		// it silently waits on for ever.
		return suggested.isEmpty ? "audio" : suggested
	}

	/// Delegate callbacks arrive on the session's own queue; everything they
	/// feed is `@MainActor` state.
	private func report(_ body: @escaping @MainActor () -> Void) {
		Task { @MainActor in body() }
	}
}
