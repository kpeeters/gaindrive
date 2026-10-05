//	GainDrive for iOS
//	Copyright (C) 2026 Kasper Peeters
//
//	This file is part of GainDrive, distributed under the GNU General
//	Public License version 3 or later, with the Application Distribution
//	Exception in ios/LICENSE. See LICENSE at the repository root for the
//	full text of the GPL.

import Foundation

/// Asks the server to build a track's transcode before anyone waits on it.
///
/// gaindrive transcodes a whole track to a file before sending any of it, which
/// buys a real `Content-Length` and byte ranges at the cost of several seconds
/// on the first request for a given track and quality. Landing that wait on the
/// moment the user tapped play is the worst possible place for it; doing it for
/// the *next* track while the current one plays moves it somewhere nobody is
/// looking. Since nothing is served the original unless the quality is set to
/// Original, that cost is otherwise paid on every first play.
///
/// An `actor` because it owns one mutable set and nothing else, and none of its
/// work belongs on the main thread. It knows nothing about the registry, the
/// settings or which server is current: it is handed a built `StreamTarget`,
/// which is what already carries the credentials and that track's own account
/// cap.
actor TranscodePrewarmer {
	/// Cache keys warmed, or being warmed, this process.
	///
	/// Transitions fire more than once for the same track - a repeat, a seek
	/// back across a boundary - and a second request would be wasted even
	/// though the server would answer it from its own cache.
	private var attempted: Set<String> = []
	/// The server's `X-Gaindrive-Sent` for each warmed cache key: the one
	/// account of what a receiver is sent, since nothing here decodes it.
	private var sentByKey: [String: String] = [:]
	private let session: URLSession

	/// `HTTP.media`, not `HTTP.shared`: waiting out the build is the entire
	/// point, and `shared` gives up after 20 s. See its comment.
	init(session: URLSession = HTTP.media) {
		self.session = session
	}

	func warm(_ target: StreamTarget) async {
		// Without a `format` the file is served straight off disk with no
		// ffmpeg involved, so there is no transcode to build and nothing to
		// wait for. That covers every film too: the server sends it as the
		// file or as a stream made while it is read. The URL rather than the
		// quality, because a cast of an original the receiver cannot play
		// says `.original` and still asks for FLAC.
		//
		// Not warmed, and accepted: an original this device cannot play, which
		// the server converts to FLAC on the strength of `playable` alone. It
		// is rare, and the wait happens when the track is reached.
		guard hasFormat(target.url) else { return }
		// A stream from an offset is piped and builds no cache file, so there
		// is nothing to warm, and a GET would read the whole of it.
		guard !hasTimeOffset(target.url) else { return }

		// Offline mode is skipped by the caller, `PlayerConnection.prewarmNext`,
		// since this actor does not see the settings.
		//
		// A track already on disk is handled a step earlier too, in
		// `PlayerConnection.streamTarget`: a stored track comes back with a
		// file URL, and a `file:` URL is not something to warm. That is why
		// there is no check for it here.

		// Claimed only now that a request is actually going out, and keyed by
		// the **cache key** rather than the ref: what gets warmed is a track at
		// a quality, so changing the quality should warm it again. Claiming it
		// before the checks above would instead make a track skipped for a
		// passing reason permanently ineligible.
		let claim = target.cacheKey
		guard attempted.insert(claim).inserted else { return }

		// One byte is enough: `Streamer::serve` runs the transcode to
		// completion before it reaches the code that honours the range, so the
		// cache is warm however little of the response is asked for. The body
		// is discarded - the point is the work the server does on the way to
		// producing it.
		// Belt and braces for the sentence above: a file URL means the bytes
		// are already here, and asking `URLSession` to range-request one would
		// be a request that means nothing.
		guard target.url.isFileURL == false else { return }

		var request = URLRequest(url: target.url)
		request.setValue("bytes=0-0", forHTTPHeaderField: "Range")
		do {
			let (_, response) = try await session.data(for: request)
			if let sent = (response as? HTTPURLResponse)?
				.value(forHTTPHeaderField: "X-Gaindrive-Sent")
			{
				sentByKey[claim] = sent
			}
		} catch {
			// Nothing here is worth interrupting playback for: the only cost of
			// failing is that the wait happens when the track is reached, which
			// is what happened before this existed. Dropped from the set so a
			// track missed while the network was down can be warmed later -
			// which covers cancellation too, since a cancelled warm has done
			// none of the work it claimed.
			attempted.remove(claim)
		}
	}

	/// What the server said it sent for a warmed key; nil when that warm has
	/// not answered, or the server predates the header.
	func sent(for cacheKey: String) -> String? {
		sentByKey[cacheKey]
	}
}

/// Whether a URL names a `format`, which is what makes the server transcode.
func hasFormat(_ url: URL) -> Bool {
	URLComponents(url: url, resolvingAgainstBaseURL: false)?
		.queryItems?.contains { $0.name == "format" } == true
}
