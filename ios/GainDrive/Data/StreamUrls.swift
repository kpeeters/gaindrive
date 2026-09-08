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

	/// A film, which is a different question from a track.
	///
	/// **Neither `format` nor `maxBitRate` goes on a video URL**, and that is
	/// not tidiness. `format` is validated against the *audio* target table, so
	/// naming an audio one is the server's switch for sending the soundtrack
	/// alone — a film played through `target(for:)` above comes back as sound
	/// with no picture, which is what happened until this existed. And
	/// `maxBitRate` sets `constrained` server-side, which disqualifies both the
	/// direct and the remux tiers and forces a full re-encode of a file that
	/// could have been served off disk.
	///
	/// The transport follows `nativeSeek`, **trusted as given**: it is false on
	/// a video reached through search, a playlist or starred, because the codec
	/// columns it is computed from are not selected by those queries. That is
	/// the safe direction — the film plays and seeks by re-request.
	/// `transcoded` forces the HLS transport for a film the server says can be
	/// served untouched.
	///
	/// **Because `nativeSeek` answers a browser's question, not this one.** It
	/// derives from `video_direct_playable()`, built from `browser_video_codec()`
	/// — and a browser plays AV1 anywhere because Chrome and Firefox bundle
	/// dav1d and decode in software. AVFoundation ships **no** software AV1
	/// decoder: decode is hardware-only, arrived with the M3 family and A17 Pro,
	/// and there is no fallback on anything older. A yt-dlp download is
	/// frequently AV1, deliberately — forcing H.264 would cap YouTube at 1080p —
	/// so this is a common file rather than an exotic one, and the symptom is a
	/// film that plays its sound over an audio placeholder with nothing
	/// anywhere saying why.
	///
	/// The HLS tier is the server re-encoding to H.264, which always plays. It
	/// costs a re-encode, so it is only ever reached by `LocalEngine` having
	/// *asked AVFoundation* and been told the track cannot be decoded.
	static func video(for song: Song, client: SubsonicClient, transcoded: Bool = false)
		-> StreamTarget
	{
		let url =
			song.nativeSeek && !transcoded
			? client.url("stream", parameters: ["id": song.ref.id])
			// `.m3u8` rather than `.view`: the server answers both, and the
			// extension is how AVFoundation knows it is a playlist. That is
			// what `SubsonicClient.url`'s `suffix` has been there for since
			// phase 1.
			: client.url("hls", suffix: ".m3u8", parameters: ["id": song.ref.id])
		return StreamTarget(
			url: url,
			// Nothing was asked for and nothing is stored: video is never
			// cached, so the key names a file that will not exist.
			quality: .original,
			cacheKey: CacheKeys.of(song.ref, quality: .original),
			contentType: nil)
	}
}
