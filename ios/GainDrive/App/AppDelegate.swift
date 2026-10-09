//	GainDrive for iOS
//	Copyright (C) 2026 Kasper Peeters
//
//	This file is part of GainDrive, distributed under the GNU General
//	Public License version 3 or later, with the Application Distribution
//	Exception in ios/LICENSE. See LICENSE at the repository root for the
//	full text of the GPL.

#if os(iOS)
	import UIKit
#else
	import Foundation
#endif

/// The one thing a SwiftUI `App` cannot do for itself.
///
/// A background `URLSession` finishing while the app is not running makes the
/// system **launch it** and call this, handing over a completion handler. Until
/// that handler is called back the system considers the app still working; not
/// calling it makes the app look unresponsive and the system stops waking it
/// for downloads at all - which is a failure that shows up as "downloads only
/// progress while the app is open", days later, with nothing in the logs.
///
/// It is the whole reason `@UIApplicationDelegateAdaptor` is here, and it is
/// deliberately the only thing in it: the composition root is
/// `GainDriveApp.init` and should stay there.
final class AppDelegate: NSObject {
	/// Set by `GainDriveApp` once the queue exists. Reached before that only if
	/// the system woke us for a session we have not built yet, in which case
	/// the handler is held until it is.
	@MainActor static var queue: DownloadQueue?
	@MainActor static var films: FilmQueue?
	/// Per session identifier: music and films are two sessions, and either
	/// can wake the app first.
	@MainActor private static var pending: [String: () -> Void] = [:]

	/// Called by `GainDriveApp` as soon as the queues are built, so a handler
	/// that arrived first is not dropped.
	@MainActor
	static func adopt(_ queue: DownloadQueue, films: FilmQueue) {
		self.queue = queue
		self.films = films
		if let handler = pending.removeValue(forKey: DownloadQueue.identifier) {
			queue.backgroundCompletion = { handler() }
		}
		if let handler = pending.removeValue(forKey: FilmQueue.identifier) {
			films.backgroundCompletion = { handler() }
		}
		queue.resume()
		films.resume()
	}
}

/// iOS only. The Mac never suspends an app, so there is no background-session
/// hand-off to receive there; `adopt` still resumes the session on both.
#if os(iOS)
	extension AppDelegate: UIApplicationDelegate {
		func application(
			_ application: UIApplication,
			handleEventsForBackgroundURLSession identifier: String,
			completionHandler: @escaping () -> Void
		) {
			MainActor.assumeIsolated {
				if identifier == FilmQueue.identifier, let films = Self.films {
					films.backgroundCompletion = { completionHandler() }
				} else if identifier == DownloadQueue.identifier, let queue = Self.queue {
					queue.backgroundCompletion = { completionHandler() }
				} else {
					Self.pending[identifier] = completionHandler
				}
			}
		}

	}
#endif

