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

	/// **The existing query has to survive**, and all of it. `paced(_:)` must
	/// not lose anything it is handed, whatever that is — which by the time a
	/// cast URL is finished is a `castToken` rather than the credentials tested
	/// here, since `withCastToken(_:_:)` swaps those out afterwards. The
	/// assertion is about `paced` keeping its hands to itself, not about which
	/// credential travels.
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

	// MARK: - Credentials

	/// Swapping this account's password out of a URL a television will fetch.
	///
	/// Silent and remote, like the pacing above, and worse in the same way:
	/// leaving `u`/`t`/`s` on means the cast works perfectly and the password
	/// goes to the receiver anyway, so nothing ever draws attention to it.
	private let ordinary =
		"https://music.example.com/rest/stream.view?id=42&u=kasper&t=abc&s=xyz&v=1.16.1&c=gaindrive&f=json"

	@Test func theTokenReplacesTheCredentials() throws {
		let swapped = withCastToken(try url(ordinary), "deadbeef")
		let items = try #require(
			URLComponents(url: swapped, resolvingAgainstBaseURL: false)?.queryItems)
		#expect(items.first { $0.name == "castToken" }?.value == "deadbeef")
		#expect(!items.contains { $0.name == "u" })
		#expect(!items.contains { $0.name == "t" })
		#expect(!items.contains { $0.name == "s" })
	}

	/// The id is what the grant is scoped against, so losing it turns a valid
	/// token into a refused one; `pace` answers a different question and a cast
	/// URL needs both.
	@Test func theIdAndThePacingSurvive() throws {
		let swapped = withCastToken(paced(try url(ordinary)), "deadbeef")
		let items = try #require(
			URLComponents(url: swapped, resolvingAgainstBaseURL: false)?.queryItems)
		#expect(items.first { $0.name == "id" }?.value == "42")
		#expect(items.first { $0.name == "pace" }?.value == "true")
	}

	/// Kept deliberately: the server ignores them on a grant-authed request, and
	/// `c` is what names this client in its log.
	@Test func theProtocolParametersSurvive() throws {
		let items = try #require(
			URLComponents(url: withCastToken(try url(ordinary), "x"), resolvingAgainstBaseURL: false)?
				.queryItems)
		#expect(items.first { $0.name == "v" }?.value == "1.16.1")
		#expect(items.first { $0.name == "c" }?.value == "gaindrive")
		#expect(items.first { $0.name == "f" }?.value == "json")
	}

	/// Two tokens would be a URL the server reads one arbitrary half of.
	@Test func aSecondApplicationReplacesRatherThanAppends() throws {
		let twice = withCastToken(withCastToken(try url(ordinary), "aaa"), "bbb")
		let items = try #require(
			URLComponents(url: twice, resolvingAgainstBaseURL: false)?.queryItems)
		#expect(items.filter { $0.name == "castToken" }.count == 1)
		#expect(items.first { $0.name == "castToken" }?.value == "bbb")
	}

	/// **Nil is the ordinary answer from a server too old to mint one**, and it
	/// has to leave the URL exactly as it was — that URL still carries the
	/// credentials and still works, which is what this app did before the
	/// endpoint existed.
	@Test func noTokenLeavesTheUrlAlone() throws {
		let original = try url(ordinary)
		#expect(withCastToken(original, nil) == original)
		#expect(withCastToken(original, "") == original)
	}

	@Test func theHostAndPathAreLeftAlone() throws {
		let swapped = withCastToken(try url("http://192.0.2.9:4040/rest/stream.view?id=7&t=abc"), "x")
		#expect(swapped.host() == "192.0.2.9")
		#expect(swapped.port == 4040)
		#expect(swapped.path() == "/rest/stream.view")
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
