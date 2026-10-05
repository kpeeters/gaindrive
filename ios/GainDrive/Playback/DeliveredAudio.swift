//	GainDrive for iOS
//	Copyright (C) 2026 Kasper Peeters
//
//	This file is part of GainDrive, distributed under the GNU General
//	Public License version 3 or later, with the Application Distribution
//	Exception in ios/LICENSE. See LICENSE at the repository root for the
//	full text of the GPL.

import AVFoundation
import CoreMedia

/// What a track's audio actually arrived as.
///
/// A different question from what was asked for: a file this device plays
/// that fits under the ceiling comes as stored, whatever format was named,
/// and an original it cannot play comes as FLAC. Answered by the player
/// where there is one (`probe`), and by the server's `X-Gaindrive-Sent`
/// header where there is not - a receiver decodes nothing here.
struct DeliveredAudio: Hashable, Sendable {
	/// As a person would say it: "FLAC", "AAC", "MP3".
	let codec: String
	let kbps: Int?
	let sampleRate: Double?
	/// Nil when there is nothing to tell it by.
	let converted: Bool?

	var label: String {
		var parts = [codec]
		if let kbps, kbps > 0 { parts.append("\(kbps) kbps") }
		if let sampleRate, sampleRate > 0 {
			parts.append(String(format: "%g kHz", (sampleRate / 100).rounded() / 10))
		}
		switch converted {
		case true?: parts.append("converted")
		case false?: parts.append("as stored")
		case nil: break
		}
		return parts.joined(separator: ", ")
	}

	/// From the server's own account of what it sent. `original` is the
	/// stored file, described by the song since the header does not repeat it.
	init?(sent: String, song: Song) {
		let words = sent.split(separator: " ").map(String.init)
		guard let name = words.first else { return nil }
		if name == "original" {
			guard let suffix = song.suffix, !suffix.isEmpty else { return nil }
			self.init(
				codec: Self.codecName(suffix: suffix), kbps: song.bitRate, sampleRate: nil,
				converted: false)
			return
		}
		self.init(
			codec: Self.codecName(suffix: name),
			kbps: words.count > 1 ? Int(words[1]) : nil, sampleRate: nil, converted: true)
	}

	init(codec: String, kbps: Int?, sampleRate: Double?, converted: Bool?) {
		self.codec = codec
		self.kbps = kbps
		self.sampleRate = sampleRate
		self.converted = converted
	}

	/// What AVFoundation says the item's audio track is.
	///
	/// "Converted" is judged against the song: a different codec from the one
	/// the stored file can hold, or the same lossy codec at well under its
	/// bitrate - an MP3 over the ceiling re-encoded as MP3. The estimate is
	/// AVFoundation's and only roughly the encoder's, hence the margin.
	static func probe(_ asset: AVAsset, song: Song) async -> DeliveredAudio? {
		// A failure here is a track not yet readable, not a fact about it;
		// the caller falls back to what was asked for.
		guard let track = try? await asset.loadTracks(withMediaType: .audio).first,
			let loaded = try? await track.load(.formatDescriptions, .estimatedDataRate),
			let description = loaded.0.first
		else { return nil }
		let dataRate = loaded.1
		let subtype = CMFormatDescriptionGetMediaSubType(description)
		let codec = codecName(subtype: subtype)
		let basic = CMAudioFormatDescriptionGetStreamBasicDescription(description)?.pointee
		let lossless = ["FLAC", "ALAC", "PCM"].contains(codec)
		let kbps = !lossless && dataRate > 0 ? Int((dataRate / 1000).rounded()) : nil

		var converted: Bool?
		if let suffix = song.suffix, !suffix.isEmpty {
			let stored = storedCodecs(suffix: suffix)
			if !stored.isEmpty {
				if !stored.contains(codec) {
					converted = true
				} else if let kbps, let source = song.bitRate, source > 0 {
					converted = Double(kbps) < Double(source) * 0.8
				} else {
					converted = false
				}
			}
		}
		return DeliveredAudio(
			codec: codec, kbps: kbps,
			sampleRate: basic.map { $0.mSampleRate }, converted: converted)
	}

	/// A server `format` name, or a file suffix, as a codec name.
	static func codecName(suffix: String) -> String {
		switch suffix.lowercased() {
		case "m4a", "aac": "AAC"
		case "mp3": "MP3"
		case "flac": "FLAC"
		case "opus": "Opus"
		case "ogg", "oga", "vorbis": "Vorbis"
		case "wav": "PCM"
		case let other: other.uppercased()
		}
	}

	/// What a file with this suffix can hold. Empty when the suffix does not
	/// say, which leaves "converted" unknown rather than guessed.
	private static func storedCodecs(suffix: String) -> Set<String> {
		switch suffix.lowercased() {
		case "mp3": ["MP3"]
		case "flac": ["FLAC"]
		case "m4a", "mp4": ["AAC", "ALAC"]
		case "aac": ["AAC"]
		case "wav": ["PCM"]
		// Apple cannot play Ogg, so an Ogg file that plays was converted.
		case "ogg", "oga", "opus": ["Vorbis", "Opus", "FLAC"]
		default: []
		}
	}

	private static func codecName(subtype: FourCharCode) -> String {
		switch subtype {
		case kAudioFormatFLAC: "FLAC"
		case kAudioFormatAppleLossless: "ALAC"
		case kAudioFormatMPEGLayer3: "MP3"
		case kAudioFormatMPEG4AAC, kAudioFormatMPEG4AAC_HE, kAudioFormatMPEG4AAC_HE_V2,
			kAudioFormatMPEG4AAC_LD, kAudioFormatMPEG4AAC_ELD:
			"AAC"
		case kAudioFormatOpus: "Opus"
		case kAudioFormatLinearPCM: "PCM"
		default:
			// The four characters themselves, which is what a person would
			// search for.
			String(
				bytes: [24, 16, 8, 0].map { UInt8((subtype >> $0) & 0xFF) }, encoding: .ascii)?
				.trimmingCharacters(in: .whitespaces) ?? "Unknown"
		}
	}
}
