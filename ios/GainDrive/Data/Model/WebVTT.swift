//	GainDrive for iOS
//	Copyright (C) 2026 Kasper Peeters
//
//	This file is part of GainDrive, distributed under the GNU General
//	Public License version 3 or later, with the Application Distribution
//	Exception in ios/LICENSE. See LICENSE at the repository root for the
//	full text of the GPL.

import Foundation

/// One subtitle, and when it is on screen.
struct Cue: Hashable, Sendable {
	let start: Double
	let end: Double
	let text: String
}

/// Reading the subtitles the server sends.
///
/// **This exists because AVFoundation cannot be given one.** Its legible tracks
/// come from the asset - embedded in the container, or declared as an HLS
/// `EXT-X-MEDIA` rendition - and there is no API to attach an external file to
/// a player item. `AVMutableComposition` composes tracks out of other *assets*
/// and will not make one from a bare `.vtt`; `textStyleRules` styles cues that
/// already exist; `AVPlayerItemLegibleOutput` reads them out rather than
/// putting any in. ExoPlayer takes a `SubtitleConfiguration` beside the video
/// and merges it, and that one API is the whole difference between the two
/// ports here.
///
/// The alternative was an HLS subtitle rendition, which the server could be
/// taught to emit - but it would reach only the re-encode tier, since a direct
/// or remuxable film is served as a file and never sees a playlist. Pushing
/// those through HLS to gain captions would re-encode what could be served
/// untouched, which is exactly what `nativeSeek` exists to prevent. Drawing the
/// cues covers both transports and needs no server change at all.
///
/// One format to parse, because `getCaptions` converts srt, ass and embedded
/// tracks through ffmpeg's webvtt muxer before sending anything.
enum WebVTT {
	/// **Scans for timing lines rather than parsing blocks.**
	///
	/// A cue is the only thing in the format containing `-->`, so this skips
	/// the `WEBVTT` header, `NOTE` comments, `STYLE` blocks and optional cue
	/// identifiers without needing to recognise any of them - which is what
	/// keeps a file with something unexpected in it from losing the cues that
	/// are fine.
	static func parse(_ source: String) -> [Cue] {
		// A conversion upstream can emit CRLF, and a `\r` left on the end of a
		// timestamp makes it unparseable - which loses every cue rather than
		// one.
		let lines =
			source
			.replacingOccurrences(of: "\r\n", with: "\n")
			.replacingOccurrences(of: "\r", with: "\n")
			.components(separatedBy: "\n")

		var cues: [Cue] = []
		var index = 0
		while index < lines.count {
			guard let (start, end) = timings(lines[index]) else {
				index += 1
				continue
			}
			index += 1
			var text: [String] = []
			while index < lines.count, !lines[index].trimmingCharacters(in: .whitespaces).isEmpty {
				text.append(strippingTags(lines[index]))
				index += 1
			}
			let joined = text.joined(separator: "\n")
				.trimmingCharacters(in: .whitespacesAndNewlines)
			if !joined.isEmpty { cues.append(Cue(start: start, end: end, text: joined)) }
		}
		return cues
	}

	/// What is on screen at `time`, or nothing.
	///
	/// The end is **exclusive**, so two cues that abut do not both show for the
	/// instant they share. Genuinely overlapping cues do, joined - that is what
	/// overlapping means, and dropping one would silently lose a speaker.
	static func showing(at time: Double, in cues: [Cue]) -> String? {
		let active = cues.filter { time >= $0.start && time < $0.end }
		guard !active.isEmpty else { return nil }
		return active.map(\.text).joined(separator: "\n")
	}

	/// `hh:mm:ss.mmm` or `mm:ss.mmm`.
	///
	/// The fraction is a **decimal fraction of a second**, not a count of
	/// milliseconds - `00:00.5` is half a second. The same trap the server's
	/// `chapters.hh` calls out for the same reason.
	static func timestamp(_ raw: String) -> Double? {
		let parts = raw.trimmingCharacters(in: .whitespaces).components(separatedBy: ":")
		guard (2...3).contains(parts.count) else { return nil }
		var seconds = 0.0
		for field in parts.dropLast() {
			guard let value = Double(field) else { return nil }
			seconds = seconds * 60 + value
		}
		guard let last = Double(parts[parts.count - 1]) else { return nil }
		return seconds * 60 + last
	}

	private static func timings(_ line: String) -> (Double, Double)? {
		let halves = line.components(separatedBy: "-->")
		guard halves.count == 2 else { return nil }
		// **Cue settings follow the end time on the same line**, space
		// separated - `line:0 position:20%`. Handing the whole remainder to the
		// timestamp parser fails, and failing here drops the cue rather than
		// the setting.
		let end = halves[1].trimmingCharacters(in: .whitespaces)
			.components(separatedBy: " ").first ?? ""
		guard let start = timestamp(halves[0]), let finish = timestamp(end) else { return nil }
		return (start, finish)
	}

	/// ffmpeg's webvtt muxer carries inline markup through - `<i>`, and a
	/// `<v Name>` voice span. Nothing here renders it, so showing it literally
	/// would be worse than dropping it.
	private static func strippingTags(_ line: String) -> String {
		line.replacingOccurrences(of: "<[^>]+>", with: "", options: .regularExpression)
	}
}
