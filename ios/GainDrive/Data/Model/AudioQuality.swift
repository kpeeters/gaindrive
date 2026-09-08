//	GainDrive for iOS
//	Copyright (C) 2026 Kasper Peeters
//
//	This file is part of GainDrive, distributed under the GNU General
//	Public License version 3 or later, with the Application Distribution
//	Exception in ios/LICENSE. See LICENSE at the repository root for the
//	full text of the GPL.

import Foundation

/// What the server may be asked to send, mirroring `data/model/AudioQuality.kt`.
///
/// **Opus, Vorbis and Ogg are absent, and their absence is the point.** The
/// server offers all three and Android's default is Opus 160, but Apple ships
/// no Ogg demuxer — `AVPlayer` cannot play any of them. Leaving them out of the
/// enum rather than merely out of the picker means the app cannot be configured
/// into silence, and `ios/PLAN.md` is corrected to match.
enum AudioFormat: String, CaseIterable, Sendable {
	/// `raw` is the server's spelling for "no format change".
	case original = "raw"
	case m4a
	case mp3

	var label: String {
		switch self {
		case .original: "Original"
		case .m4a: "AAC"
		case .mp3: "MP3"
		}
	}

	/// Not handed to AVFoundation for a *network* URL: the server sends a
	/// correct `Content-Type` on both the cached-file and the chunked path, and
	/// `AVURLAsset` reads it, so an out-of-band hint would be a second source
	/// of truth for something already right.
	///
	/// **A local file has no header**, which is what `fileExtension` is for.
	var contentType: String? {
		switch self {
		case .original: nil
		case .m4a: "audio/mp4"
		case .mp3: "audio/mpeg"
		}
	}

	/// What a downloaded file of this format must be **named**.
	///
	/// AVFoundation determines a local file's type from its path extension —
	/// there is no header to read — and a file with none is not reported as
	/// unplayable. The player simply waits, for ever, which reads as a track
	/// that never starts rather than as a track that failed. That was a real
	/// bug: downloads landed as `<songId>@m4a160` and every one of them hung.
	///
	/// Nil for the original, whose container is whatever the server holds and
	/// is only known from the response — see `DownloadQueue`.
	var fileExtension: String? {
		switch self {
		case .original: nil
		case .m4a: "m4a"
		case .mp3: "mp3"
		}
	}
}

struct AudioQuality: Hashable, Sendable {
	let format: AudioFormat
	let bitRate: Int

	/// **The one constant to change** if the container turns out to misbehave
	/// on the server's chunked fallback.
	///
	/// AAC-LC at 160 beats MP3 at 160 audibly, and the transcode cache writes a
	/// real MP4 with a sample table, so seeking is exact rather than estimated —
	/// which ADTS, having no index at all, cannot offer. The objection to MP4 is
	/// that ffmpeg writes its index at the end and a pipe cannot seek back; the
	/// server already answers it by adding `frag_keyframe+empty_moov` on the
	/// pipe path (`src/streamer.cc:389`), so the fallback is fragmented MP4,
	/// which streams.
	static let `default` = AudioQuality(format: .m4a, bitRate: 160)
	static let original = AudioQuality(format: .original, bitRate: 0)

	static let bitRates = [96, 128, 160, 192, 256]

	/// Short, stable and filename-safe: this is the suffix of the cache key
	/// phase 5 will store under, so changing how it is spelled orphans every
	/// stored track.
	var tag: String {
		format == .original ? "orig" : "\(format.rawValue)\(bitRate)"
	}

	var label: String {
		format == .original ? "Original" : "\(format.label) \(bitRate)"
	}

	/// Applies an account's `maxBitRate` ceiling.
	///
	/// The `.original` branch looks cosmetic and is not: a capped account asking
	/// for the raw file is answered with **MP3 at the cap** by `streamer.cc`'s
	/// bitrate-limit branch. Without modelling that, the cache key would claim
	/// `@orig` for bytes that are really MP3, and a later request for genuinely
	/// original audio would be served them.
	func cappedBy(_ accountCap: Int) -> AudioQuality {
		guard accountCap > 0 else { return self }
		if format == .original { return AudioQuality(format: .mp3, bitRate: accountCap) }
		guard bitRate > accountCap else { return self }
		return AudioQuality(format: format, bitRate: accountCap)
	}

	static func parse(_ tag: String) -> AudioQuality? {
		if tag == "orig" { return .original }
		for format in AudioFormat.allCases where format != .original {
			guard tag.hasPrefix(format.rawValue) else { continue }
			guard let rate = Int(tag.dropFirst(format.rawValue.count)), rate > 0 else { return nil }
			return AudioQuality(format: format, bitRate: rate)
		}
		return nil
	}
}
