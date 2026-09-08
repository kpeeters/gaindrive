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

/// What a film is asked for.
struct VideoUrlTests {
	private let server = ServerId()

	private var client: SubsonicClient {
		SubsonicClient(
			serverId: server,
			baseURL: URL(string: "https://example.test")!,
			auth: AuthParameters(username: "admin", password: "secret", salt: "aa11"))
	}

	private func video(nativeSeek: Bool) -> Song {
		Song(
			ref: ItemRef(server: server, id: "42"), title: "Film", artistName: "A",
			albumTitle: "B", albumRef: nil, track: nil, discNumber: nil, year: nil,
			duration: 5400, bitRate: nil, suffix: "mkv", contentType: nil, sizeBytes: 0,
			coverArt: nil, starredAt: nil, lastPlayedAt: nil, isVideo: true,
			nativeSeek: nativeSeek, width: 1920, height: 1080)
	}

	/// **Neither parameter, ever.** `format` is validated against the *audio*
	/// target table, so naming an audio one is the server's switch for sending
	/// the soundtrack alone — a film played through the audio URL comes back as
	/// sound with no picture, which is what happened until this existed. And
	/// `maxBitRate` sets `constrained` server-side, forcing a full re-encode of
	/// a file that could have been served off disk.
	@Test func aVideoUrlCarriesNoFormatAndNoCeiling() {
		for seekable in [true, false] {
			let url = StreamUrls.video(for: video(nativeSeek: seekable), client: client).url
				.absoluteString
			#expect(!url.contains("format="))
			#expect(!url.contains("maxBitRate="))
		}
	}

	/// Trusted as given: `nativeSeek` is false on a video reached through
	/// search, a playlist or starred, because the codec columns it is computed
	/// from are not selected by those queries. That is the safe direction — the
	/// film plays and seeks by re-request.
	@Test func nativeSeekPicksTheTransport() {
		let direct = StreamUrls.video(for: video(nativeSeek: true), client: client).url
		#expect(direct.path.hasSuffix("stream.view"))

		let hls = StreamUrls.video(for: video(nativeSeek: false), client: client).url
		// The extension, not `.view`: it is how AVFoundation knows it is a
		// playlist, and the server answers both spellings.
		#expect(hls.path.hasSuffix("hls.m3u8"))
	}
}
