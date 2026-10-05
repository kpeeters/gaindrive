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

/// The stream URL, asserted directly - the same style as `AuthParametersTests`
/// and `CoverUrlsTests`, and possible for the same reason: the builder is a
/// pure function, so its return value *is* the request.
struct StreamUrlsTests {
	private let server = ServerId()

	private func client(salt: String = "aa11") -> SubsonicClient {
		SubsonicClient(
			serverId: server,
			baseURL: URL(string: "http://192.0.2.9:4040")!,
			auth: AuthParameters(username: "admin", password: "secret", salt: salt))
	}

	private func query(_ url: URL) -> [String: String] {
		let items = URLComponents(url: url, resolvingAgainstBaseURL: false)?.queryItems ?? []
		return Dictionary(items.compactMap { item in item.value.map { (item.name, $0) } }) {
			first, _ in first
		}
	}

	private func target(_ quality: AudioQuality = .default, id: String = "42") -> StreamTarget {
		StreamUrls.target(for: ItemRef(server: server, id: id), client: client(), quality: quality)
	}

	@Test func addressesTheStreamEndpoint() {
		let built = target()
		#expect(built.url.path() == "/rest/stream.view")
		#expect(query(built.url)["id"] == "42")
	}

	/// **The 320 kbps trap.** A format change with no `maxBitRate` is served at
	/// 320, so sending `format` alone would quietly deliver 320 for the whole
	/// library while the app believed it had asked for 160.
	@Test func formatAndBitrateAlwaysTravelTogether() {
		let parameters = query(target().url)
		#expect(parameters["format"] == "m4a")
		#expect(parameters["maxBitRate"] == "160")
	}

	/// For the original the server serves the file with no ffmpeg at all, and
	/// sending a bitrate would silently turn that into an MP3 transcode.
	@Test func theOriginalSendsNeither() {
		let parameters = query(target(.original).url)
		#expect(parameters["format"] == nil)
		#expect(parameters["maxBitRate"] == nil)
		#expect(parameters["id"] == "42")
	}

	/// `timeOffset` bypasses the transcode cache - every seek would become a
	/// fresh ffmpeg run over a response that cannot then be seeked.
	/// `estimateContentLength` promises a length ffmpeg pads or truncates to,
	/// with ranges already cleared, so a range request mis-seeks in silence.
	@Test func neitherSeekParameterIsEverSent() {
		for quality in [AudioQuality.default, .original, AudioQuality(format: .mp3, bitRate: 96)] {
			let parameters = query(target(quality).url)
			#expect(parameters["timeOffset"] == nil)
			#expect(parameters["estimateContentLength"] == nil)
		}
	}

	@Test func theTokenSchemeStillApplies() {
		let built = target()
		#expect(query(built.url)["u"] == "admin")
		#expect(query(built.url)["p"] == nil)
		#expect(!built.url.absoluteString.contains("secret"))
	}

	// MARK: - The cache key

	@Test func theCacheKeyNamesTheRefAndTheQuality() {
		let built = target()
		#expect(built.cacheKey == "\(server.value.uuidString)/42@m4a160")
	}

	/// The key must not contain the salt, or phase 5's byte cache misses on
	/// every launch - the same trap `CoverSource` exists to avoid.
	@Test func theCacheKeyIsStableAcrossSessions() {
		let first = StreamUrls.target(
			for: ItemRef(server: server, id: "42"), client: client(salt: "aaaa"),
			quality: .default)
		let second = StreamUrls.target(
			for: ItemRef(server: server, id: "42"), client: client(salt: "bbbb"),
			quality: .default)

		#expect(first.url != second.url)
		#expect(first.cacheKey == second.cacheKey)
	}

	/// The URL and the key must move together: bytes filed under a name that
	/// promises a different quality cannot be detected downstream.
	@Test func aCeilingChangesTheUrlAndTheKeyTogether() {
		let at160 = target()
		let at96 = target(AudioQuality(format: .m4a, bitRate: 96))
		#expect(at160.url != at96.url)
		#expect(at160.cacheKey != at96.cacheKey)
		#expect(at96.cacheKey.hasSuffix("@m4a96"))
	}

	@Test func theSameSongOnTwoServersGivesTwoKeys() {
		let other = ServerId()
		let elsewhere = StreamUrls.target(
			for: ItemRef(server: other, id: "42"),
			client: SubsonicClient(
				serverId: other, baseURL: URL(string: "http://other:4040")!,
				auth: AuthParameters(username: "admin", password: "secret", salt: "aa11")),
			quality: .default)
		#expect(target().cacheKey != elsewhere.cacheKey)
	}
}
