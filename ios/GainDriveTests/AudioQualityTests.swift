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

	// MARK: - Capping

	@Test func noCapChangesNothing() {
		#expect(AudioQuality.default.cappedBy(0) == .default)
		#expect(AudioQuality.original.cappedBy(0) == .original)
	}

	@Test func aCapBelowTheRequestedBitrateLowersIt() {
		#expect(
			AudioQuality(format: .m4a, bitRate: 256).cappedBy(128)
				== AudioQuality(format: .m4a, bitRate: 128))
	}

	@Test func aCapAboveTheRequestedBitrateChangesNothing() {
		#expect(AudioQuality(format: .m4a, bitRate: 96).cappedBy(320).bitRate == 96)
	}

	/// The branch that looks cosmetic and is not: `streamer.cc`'s bitrate-limit
	/// path answers a raw request on a capped account with **MP3 at the cap**.
	/// Modelling anything else would file MP3 bytes under a key promising the
	/// original.
	@Test func theOriginalUnderACapBecomesMP3() {
		let capped = AudioQuality.original.cappedBy(128)
		#expect(capped == AudioQuality(format: .mp3, bitRate: 128))
		#expect(capped.tag == "mp3128")
	}

	@Test func labelsReadAsAPersonWouldSayThem() {
		#expect(AudioQuality.original.label == "Original")
		#expect(AudioQuality(format: .m4a, bitRate: 160).label == "AAC 160")
		#expect(AudioQuality(format: .mp3, bitRate: 96).label == "MP3 96")
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
}
