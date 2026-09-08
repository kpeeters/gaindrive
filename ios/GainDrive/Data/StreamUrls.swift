//	GainDrive for iOS
//	Copyright (C) 2026 Kasper Peeters
//
//	This file is part of GainDrive, distributed under the GNU General
//	Public License version 3 or later, with the Application Distribution
//	Exception in ios/LICENSE. See LICENSE at the repository root for the
//	full text of the GPL.

import Foundation

/// A stream to play, the quality it really is, and the name those bytes go
/// under.
///
/// The three travel together on purpose. A URL naming one quality paired with a
/// key naming another stores bytes that a later request will be served
/// expecting something else, and nothing downstream can detect it.
struct StreamTarget: Hashable, Sendable {
	let url: URL
	let quality: AudioQuality
	let cacheKey: String
	let contentType: String?
}

/// `<serverId>/<songId>@<qualityTag>`.
///
/// **Never the URL.** A stream URL carries a per-session auth salt, so a
/// URL-derived key would miss after every launch — the same trap
/// `CoverSource.cacheKey` exists to avoid.
enum CacheKeys {
	static func of(_ ref: ItemRef, quality: AudioQuality) -> String {
		"\(ref.encoded)@\(quality.tag)"
	}

	/// The inverse, which a background download needs.
	///
	/// A `URLSessionDownloadTask` outlives the process, and its delegate
	/// callbacks arrive after a relaunch with nothing but the task — so the key
	/// travels in `taskDescription` and has to survive the round trip. There is
	/// no in-memory map that could do this job.
	///
	/// Split on the **last** `@`: a quality tag never contains one, while a
	/// Subsonic id is a string somebody else chose and might.
	static func parse(_ key: String) -> (ref: ItemRef, quality: AudioQuality)? {
		guard let at = key.lastIndex(of: "@") else { return nil }
		guard let ref = ItemRef(encoded: String(key[key.startIndex..<at])) else { return nil }
		guard let quality = AudioQuality.parse(String(key[key.index(after: at)...])) else {
			return nil
		}
		return (ref, quality)
	}
}

/// Builds the URL a track is played from.
///
/// A **pure function**, deliberately: it is the whole of the stream policy, and
/// being pure is what lets it be tested by calling it rather than by observing
/// a request — the same shape as `SubsonicClient.url`, `AuthParameters` and
/// `CoverUrls`.
enum StreamUrls {
	/// The account ceiling is passed in rather than looked up, because it
	/// belongs to **this track's** server. A queue may span servers and crosses
	/// caps at every boundary, so there is no single "current" cap to read and
	/// nothing here may close over one.
	static func target(
		for ref: ItemRef, client: SubsonicClient, wanted: AudioQuality, accountCap: Int
	) -> StreamTarget {
		let quality = wanted.cappedBy(accountCap)
		var parameters = ["id": ref.id]

		if quality.format != .original {
			// **Both, always.** A format change with no `maxBitRate` is served
			// at 320 kbps (`src/streamer.cc:95`), so sending `format` alone
			// would quietly deliver 320 for the whole library while the app
			// believed it had asked for 160.
			parameters["format"] = quality.format.rawValue
			parameters["maxBitRate"] = String(quality.bitRate)
		}
		// For the original neither is sent: the server serves the file with no
		// ffmpeg involved at all.
		//
		// Two more are never sent, and both omissions are load-bearing.
		// `timeOffset` bypasses the transcode cache entirely, turning every
		// seek into a fresh ffmpeg run over a chunked response that cannot then
		// be seeked. `estimateContentLength` promises a length ffmpeg
		// zero-pads or truncates to, with ranges already cleared — a range
		// request gets a 200 from byte 0 and the player mis-seeks in silence.
		// The duration comes from `Song.duration`, which the server already
		// gave us and which is exact.

		return StreamTarget(
			url: client.url("stream", parameters: parameters),
			quality: quality,
			cacheKey: CacheKeys.of(ref, quality: quality),
			contentType: quality.format.contentType)
	}
}
