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

/// The declaration that stops the server remuxing a container AVFoundation
/// demuxes, and — the half worth testing — its absence everywhere else.
///
/// The same reasoning as `CastUrlTests`: the failure is silent and remote. A
/// cast URL carrying `playableContainers` gets the receiver a `LOAD`
/// announcing `video/mp4` followed by QuickTime, which it refuses outright.
/// The film never starts and nothing on the phone says why. The obvious
/// refactor — filling the set in inside `StreamUrls.video` "because both
/// callers want it" — is exactly that bug, and `castRouteDeclaresNothing`
/// below is what stands in its way.
struct PlayableContainersTests {
	@Test func declaredSetBecomesOneSortedList() {
		// Sorted, so a Set's iteration order cannot make one request build two
		// different URLs — they reach a log line and a URL cache.
		#expect(
			StreamUrls.videoParameters(id: "7", containers: ["mov", "avi"])
				== ["id": "7", "playableContainers": "avi,mov"])
	}

	@Test func orderOfTheSetDoesNotReachTheURL() {
		#expect(
			StreamUrls.videoParameters(id: "7", containers: ["mov", "avi"])
				== StreamUrls.videoParameters(id: "7", containers: ["avi", "mov"]))
	}

	/// The cast route passes no set at all, and this is the assertion that
	/// catches it being given one.
	@Test func castRouteDeclaresNothing() {
		#expect(
			StreamUrls.videoParameters(id: "7", containers: []) == ["id": "7"])
	}

	/// Matroska is the container this cannot claim. AVFoundation does not
	/// demux it at any version, so declaring it would trade a wait for a film
	/// that does not play — and `.mkv` is the common case, which is what makes
	/// the temptation real.
	@Test func matroskaIsNeverDeclared() {
		#expect(!avfoundationContainers.contains("mkv"))
		#expect(!avfoundationContainers.contains("webm"))
	}

	/// A DVD titleset is one stream split across numbered VOBs and the stored
	/// path names only the first, so serving it untouched hands back twenty
	/// minutes of a two-hour film. The server refuses the declaration; this
	/// records the same rule on the side that would make it.
	@Test func vobIsNeverDeclared() {
		#expect(!avfoundationContainers.contains("vob"))
	}

	/// mp4 and m4v are served untouched to every client already, so declaring
	/// them would say nothing. Keeping them out is what makes the set mean
	/// "more than a browser takes".
	@Test func containersTheServerAlreadyServesDirectlyAreAbsent() {
		#expect(!avfoundationContainers.contains("mp4"))
		#expect(!avfoundationContainers.contains("m4v"))
	}
}
