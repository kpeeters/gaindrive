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

/// The audio half of `playable`: what this device says it can play exactly as
/// the server holds it, and - the half worth testing - what it must never say.
///
/// Declaring too little wastes a re-encode, which is merely slow. Declaring Ogg
/// is the failure peculiar to this platform: Apple ships no demuxer for it, so
/// the server would hand over a file that plays as silence.
struct PlayableAudioTests {
	private let aac160 = AudioQuality(format: .m4a, bitRate: 160)

	/// Lossless is declared whatever the setting: size is `maxBitRate`'s job,
	/// and with no ceiling the declared FLAC is what an unplayable original
	/// is converted to.
	@Test func theLosslessFormatsAreDeclared() {
		#expect(avfoundationAudio.contains("flac/flac"))
		#expect(avfoundationAudio.contains("mp4/alac"))
		#expect(avfoundationAudio.contains("mpeg/mp3"))
		#expect(avfoundationAudio.contains("mp4/aac"))
	}

	/// **The one that is specific to this platform.** `AudioFormat` leaves Opus
	/// and Vorbis out of the enum so the app cannot be *configured* into
	/// silence; this is the same guard on the other side, where the server
	/// would be *told* to send it. Android declares both and is right to.
	@Test func noTokenNamesOgg() {
		for token in avfoundationAudio {
			#expect(!token.hasPrefix("ogg/"), "declared \(token)")
		}
	}

	/// Every audio token is a `container/codec` pair. A bare one would be read
	/// by the server as a *video* container and match nothing at all -
	/// silently, which is why this is asserted rather than left to the reader.
	@Test func everyTokenIsOneContainerOverOneCodec() {
		for token in avfoundationAudio {
			#expect(token == token.lowercased(), "\(token) is not lowercase")
			#expect(token.filter { $0 == "/" }.count == 1, "\(token) is not one pair")
			#expect(!token.hasPrefix("/") && !token.hasSuffix("/"), "\(token) has an empty half")
		}
	}

	/// The container is the real one, never a file extension: the server stores
	/// what its scan observed, so `.m4a` is `mp4` and a token naming the
	/// extension would reach nothing.
	@Test func noTokenNamesAFileExtension() {
		let containers = Set(
			avfoundationAudio
				.map { $0.prefix(while: { $0 != "/" }) }
				.map(String.init))
		for extensionName in ["m4a", "oga", "mp3", "opus", "wav", "wma"] {
			#expect(!containers.contains(extensionName), "\(extensionName) is an extension")
		}
	}

	// ---- the query itself ------------------------------------------------

	@Test func aDeclaredSetBecomesOneSortedList() {
		#expect(
			StreamUrls.audioParameters(
				id: "7", quality: aac160, playable: ["mpeg/mp3", "adts/aac"])
				== [
					"id": "7", "format": "m4a", "maxBitRate": "160",
					"playable": "adts/aac,mpeg/mp3",
				])
	}

	@Test func orderOfTheSetDoesNotReachTheURL() {
		#expect(
			StreamUrls.audioParameters(
				id: "7", quality: aac160, playable: ["mpeg/mp3", "mp4/aac"])
				== StreamUrls.audioParameters(
					id: "7", quality: aac160, playable: ["mp4/aac", "mpeg/mp3"]))
	}

	/// The audio twin of `castRouteDeclaresNothing`. A receiver handed a URL
	/// that declares is sent the file the server holds while its `LOAD`
	/// announced the transcode's type, and refuses the media. The obvious
	/// refactor - filling the set in inside the resolver "because both callers
	/// want it" - is exactly that bug, and this is what stands in its way.
	@Test func castRouteDeclaresNothing() {
		#expect(
			StreamUrls.audioParameters(id: "7", quality: aac160, playable: [])
				== ["id": "7", "format": "m4a", "maxBitRate": "160"])
	}

	/// The original is spelled by sending no `format` at all, so a request for
	/// it carries no bitrate either - and may still declare.
	@Test func theOriginalSendsNoFormatAndNoBitrate() {
		#expect(
			StreamUrls.audioParameters(
				id: "7", quality: .original, playable: ["flac/flac"])
				== ["id": "7", "playable": "flac/flac"])
	}
}
