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

/// The two pure decisions behind a URL a receiver fetches.
///
/// Neither can be checked against hardware in any useful way — a missing `pace`
/// shows up as music stopping ninety seconds in, half a minute after the cause,
/// and a wrong codec verdict as a LOAD that fails twice and then says nothing.
/// So they are checked here.
struct CastUrlTests {
	// MARK: - Pacing

	private func url(_ text: String) throws -> URL {
		try #require(URL(string: text))
	}

	@Test func pacingIsAdded() throws {
		let paced = paced(try url("https://music.example.com/rest/stream.view?id=42"))
		#expect(paced.absoluteString.contains("pace=true"))
	}

	/// **The existing query has to survive**, and all of it: the credentials are
	/// in there, and a receiver handed a URL with `t=` dropped fetches nothing
	/// and reports an error nobody can trace back to here.
	@Test func pacingKeepsEverythingElse() throws {
		let original = "https://music.example.com/rest/stream.view?id=42&u=kasper&t=abc&s=xyz&f=json"
		let paced = paced(try url(original))
		let items = try #require(
			URLComponents(url: paced, resolvingAgainstBaseURL: false)?.queryItems)
		#expect(items.count == 6)
		#expect(items.first { $0.name == "t" }?.value == "abc")
		#expect(items.first { $0.name == "id" }?.value == "42")
		#expect(items.first { $0.name == "pace" }?.value == "true")
	}

	@Test func pacingWorksOnAUrlWithNoQuery() throws {
		let paced = paced(try url("http://192.0.2.9:4040/rest/stream.view"))
		#expect(paced.absoluteString == "http://192.0.2.9:4040/rest/stream.view?pace=true")
	}

	/// A private address with a port is the ordinary shape of a self-hosted
	/// server, and the one `NSAllowsLocalNetworking` does not cover — so it is
	/// worth knowing it survives this untouched.
	@Test func pacingLeavesTheHostAlone() throws {
		let paced = paced(try url("http://192.0.2.9:4040/rest/stream.view?id=7"))
		#expect(paced.host() == "192.0.2.9")
		#expect(paced.port == 4040)
	}

	// MARK: - What a receiver can decode

	@Test(
		arguments: [
			"audio/flac", "audio/mpeg", "audio/mp4", "audio/aac", "audio/ogg", "audio/wav",
		])
	func theDocumentedTypesPlay(_ type: String) {
		#expect(castPlaysNatively(type))
	}

	/// The one type `src/codecs.hh` can report that the Default Media Receiver
	/// does not take. Sent as it stands it fails the LOAD, is retried once by
	/// `LoadRetryWatcher`, fails again, and stalls with nothing on screen.
	@Test func windowsMediaDoesNot() {
		#expect(!castPlaysNatively("audio/x-ms-wma"))
	}

	/// **An allowlist, not a one-entry denylist.** A codec the server learns to
	/// report later must be transcoded rather than sent to a receiver that
	/// cannot play it: a needless transcode is the cheap way to be wrong.
	@Test func anUnknownTypeIsNotAssumedToPlay() {
		#expect(!castPlaysNatively("audio/webm"))
		#expect(!castPlaysNatively("audio/x-something-new"))
	}

	/// Not knowing is not the same as knowing it is fine.
	@Test func noTypeIsNotAnAnswer() {
		#expect(!castPlaysNatively(nil))
		#expect(!castPlaysNatively(""))
	}
}
