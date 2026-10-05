//	GainDrive for iOS
//	Copyright (C) 2026 Kasper Peeters
//
//	This file is part of GainDrive, distributed under the GNU General
//	Public License version 3 or later, with the Application Distribution
//	Exception in ios/LICENSE. See LICENSE at the repository root for the
//	full text of the GPL.

import Foundation
import Testing

@testable import GainDrive

struct AudioQualityTests {
	@Test func tagsRoundTrip() throws {
		for quality in [
			AudioQuality.original,
			AudioQuality(format: .m4a, bitRate: 160),
			AudioQuality(format: .mp3, bitRate: 96),
		] {
			#expect(AudioQuality.parse(quality.tag) == quality)
		}
	}

	@Test(arguments: ["", "opus160", "m4a", "m4a0", "m4ax", "nonsense"])
	func rejectsTagsWeNeverWrote(tag: String) {
		#expect(AudioQuality.parse(tag) == nil)
	}

	/// **The guard against Opus creeping back.** Apple ships no Ogg demuxer, so
	/// a default the enum does not list would be a default that plays silence.
	@Test func theDefaultIsAFormatThePlayerCanDecode() {
		#expect(AudioFormat.allCases.contains(AudioQuality.default.format))
		#expect(AudioQuality.default.format != .original)
		#expect(AudioQuality.default.bitRate > 0)
	}

	@Test func labelsReadAsAPersonWouldSayThem() {
		#expect(AudioQuality.original.label == "Original")
		#expect(AudioQuality(format: .m4a, bitRate: 160).label == "Up to 160 kbps")
		#expect(AudioQuality(format: .mp3, bitRate: 96).label == "Up to 96 kbps")
	}

	/// **Every format the app can ask for must name a file extension**, because
	/// AVFoundation types a local file by its extension and has no header to
	/// fall back on. A stored file without one is not reported as unplayable -
	/// the player waits for ever - so this is the check that keeps a new format
	/// from reintroducing a silent hang.
	@Test func everyTranscodeFormatNamesAFileExtension() {
		for format in AudioFormat.allCases where format != .original {
			#expect(format.fileExtension != nil, "\(format) has no file extension")
		}
	}

	/// The original's container is whatever the server holds, so it genuinely
	/// cannot be known here - `DownloadQueue` reads it off the response.
	@Test func theOriginalNamesNoExtension() {
		#expect(AudioFormat.original.fileExtension == nil)
	}

	/// No `format` on a video fetches the film, so a soundtrack always names
	/// a container; every other quality is honoured as set.
	@Test func aVideosSoundtrackNeverAsksForTheOriginal() {
		#expect(AudioQuality.original.forVideoAudio() == .default)
		let low = AudioQuality(format: .mp3, bitRate: 96)
		#expect(low.forVideoAudio() == low)
	}
}
