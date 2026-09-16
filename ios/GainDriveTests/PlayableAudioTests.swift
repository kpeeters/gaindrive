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

/// The audio half of `playable`: what this device says it will take exactly as
/// the server holds it, and — the half worth testing — what it must never say.
///
/// Three failures are guarded against and they fail in three different ways.
/// Declaring too little wastes a re-encode, which is merely slow. Declaring
/// something lossless at a lossy setting hands someone a FLAC over mobile data,
/// which is the opposite of what that setting asks for. Declaring Ogg is the
/// one peculiar to this platform: Apple ships no demuxer for it, so the server
/// would hand over a file that plays as silence.
struct PlayableAudioTests {
	private let aac160 = AudioQuality(format: .m4a, bitRate: 160)

	/// The rule the feature turns on, and the one a later edit is most likely
	/// to undo by putting a token in the wrong set.
	@Test func aLossySettingDeclaresNothingLossless() {
		let declared = avfoundationPlayable(for: aac160)
		#expect(declared == avfoundationAudioLossy)
		#expect(declared.isDisjoint(with: avfoundationAudioLossless))
	}

	@Test func originalDeclaresTheLosslessFormatsToo() {
		let declared = avfoundationPlayable(for: .original)
		#expect(declared.isSuperset(of: avfoundationAudioLossless))
		#expect(declared.isSuperset(of: avfoundationAudioLossy))
	}

	/// ALAC is lossless in exactly the way FLAC is, and declaring it at a lossy
	/// setting is the same mistake wearing a different container. Named
	/// individually because the two sets could be edited to agree with each
	/// other and still be wrong.
	@Test func alacIsTreatedAsLossless() {
		#expect(avfoundationAudioLossless.contains("mp4/alac"))
		#expect(avfoundationAudioLossless.contains("flac/flac"))
		#expect(!avfoundationPlayable(for: aac160).contains("mp4/alac"))
	}

	/// **The one that is specific to this platform.** `AudioFormat` leaves Opus
	/// and Vorbis out of the enum so the app cannot be *configured* into
	/// silence; this is the same guard on the other side, where the server
	/// would be *told* to send it. Android declares both and is right to.
	@Test func noTokenNamesOgg() {
		for token in avfoundationAudioLossy.union(avfoundationAudioLossless) {
			#expect(!token.hasPrefix("ogg/"), "declared \(token)")
		}
	}

	/// `cappedBy` turns a request for the original into mp3 at the cap, so
	/// reading the capped value would silently stop every capped account
	/// declaring the lossless half.
	@Test func theAccountCapIsNotWhatTheDeclarationReads() {
		let capped = AudioQuality.original.cappedBy(192)
		#expect(capped.format == .mp3)
		#expect(avfoundationPlayable(for: .original).isSuperset(of: avfoundationAudioLossless))
		#expect(!avfoundationPlayable(for: capped).isSuperset(of: avfoundationAudioLossless))
	}

	/// Every audio token is a `container/codec` pair. A bare one would be read
	/// by the server as a *video* container and match nothing at all —
	/// silently, which is why this is asserted rather than left to the reader.
	@Test func everyTokenIsOneContainerOverOneCodec() {
		for token in avfoundationAudioLossy.union(avfoundationAudioLossless) {
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
			avfoundationAudioLossy.union(avfoundationAudioLossless)
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
	/// refactor — filling the set in inside the resolver "because both callers
	/// want it" — is exactly that bug, and this is what stands in its way.
	@Test func castRouteDeclaresNothing() {
		#expect(
			StreamUrls.audioParameters(id: "7", quality: aac160, playable: [])
				== ["id": "7", "format": "m4a", "maxBitRate": "160"])
	}

	/// The original is spelled by sending no `format` at all, so a request for
	/// it carries no bitrate either — and may still declare.
	@Test func theOriginalSendsNoFormatAndNoBitrate() {
		#expect(
			StreamUrls.audioParameters(
				id: "7", quality: .original, playable: ["flac/flac"])
				== ["id": "7", "playable": "flac/flac"])
	}
}
