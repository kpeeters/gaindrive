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

/// The server's `X-Gaindrive-Sent`, read into what the track-info sheet shows
/// for a receiver, where nothing here decodes the bytes.
struct DeliveredAudioTests {
	private let server = ServerId()

	private func song(suffix: String?, bitRate: Int?) -> Song {
		Song(
			ref: ItemRef(server: server, id: "1"), title: "T", artistName: "A",
			albumTitle: "B", albumRef: nil, track: nil, discNumber: nil, year: nil,
			duration: 200, bitRate: bitRate, suffix: suffix, contentType: nil,
			sizeBytes: 0, coverArt: nil, starredAt: nil, lastPlayedAt: nil,
			isVideo: false, nativeSeek: false, width: nil, height: nil)
	}

	@Test func originalDescribesTheStoredFile() {
		let delivered = DeliveredAudio(sent: "original", song: song(suffix: "mp3", bitRate: 320))
		#expect(delivered?.label == "MP3, 320 kbps, as stored")
	}

	@Test func aLossyEncodeCarriesItsRate() {
		let delivered = DeliveredAudio(sent: "m4a 160", song: song(suffix: "flac", bitRate: 900))
		#expect(delivered?.label == "AAC, 160 kbps, converted")
	}

	@Test func aLosslessEncodeHasNoRate() {
		let delivered = DeliveredAudio(sent: "flac", song: song(suffix: "ogg", bitRate: 192))
		#expect(delivered?.label == "FLAC, converted")
	}

	/// An original with no suffix has nothing to be described by, and is
	/// left to the request rather than guessed.
	@Test func anOriginalWithoutASuffixSaysNothing() {
		#expect(DeliveredAudio(sent: "original", song: song(suffix: nil, bitRate: nil)) == nil)
	}

	@Test func theSampleRateReadsInKilohertz() {
		let delivered = DeliveredAudio(
			codec: "FLAC", kbps: nil, sampleRate: 44_100, converted: false)
		#expect(delivered.label == "FLAC, 44.1 kHz, as stored")
	}
}
