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

/// The key a download travels under.
///
/// This test exists because the failure is
/// silent: a key that does not round-trip matches nothing rather than
/// throwing, and the symptom is a download that completes and then plays from
/// the network anyway.
struct CacheKeysTests {
	private let server = ServerId()

	@Test func aKeyRoundTrips() {
		let ref = ItemRef(server: server, id: "4212")
		let quality = AudioQuality(format: .m4a, bitRate: 160)
		let key = CacheKeys.of(ref, quality: quality)
		let parsed = CacheKeys.parse(key)
		#expect(parsed?.ref == ref)
		#expect(parsed?.quality == quality)
	}

	@Test func theOriginalRoundTripsToo() {
		let ref = ItemRef(server: server, id: "1")
		let key = CacheKeys.of(ref, quality: .original)
		#expect(CacheKeys.parse(key)?.quality == AudioQuality.original)
	}

	/// **Split on the last `@`, not the first.** A quality tag never contains
	/// one; a Subsonic id is a string somebody else chose and might.
	@Test func anIdContainingTheSeparatorSurvives() {
		let ref = ItemRef(server: server, id: "we@dnesday")
		let quality = AudioQuality(format: .mp3, bitRate: 128)
		let parsed = CacheKeys.parse(CacheKeys.of(ref, quality: quality))
		#expect(parsed?.ref == ref)
		#expect(parsed?.quality == quality)
	}

	@Test func nonsenseParsesToNothingRatherThanGuessing() {
		#expect(CacheKeys.parse("") == nil)
		#expect(CacheKeys.parse("no-at-sign") == nil)
		#expect(CacheKeys.parse("not-a-uuid/1@m4a160") == nil)
		#expect(CacheKeys.parse("\(ServerId().value.uuidString)/1@nonsense") == nil)
	}

	/// The store's layout *is* the key: `Media/<serverId>/<songId>@<tag>`.
	/// A path that did not agree with the key would store bytes where nothing
	/// looks for them.
	@Test func theStorePathFollowsTheKey() {
		let root = URL(filePath: "/tmp/gaindrive-test")
		let ref = ItemRef(server: server, id: "4212")
		let quality = AudioQuality(format: .m4a, bitRate: 160)
		let path = AudioStore.path(root: root, key: CacheKeys.of(ref, quality: quality))
		#expect(path.lastPathComponent == "4212@m4a160")
		#expect(path.deletingLastPathComponent().lastPathComponent == server.description)
	}

	/// An id with a slash in it would otherwise write outside the directory it
	/// was meant for.
	@Test func anIdWithASlashCannotEscapeItsDirectory() {
		let root = URL(filePath: "/tmp/gaindrive-test")
		let ref = ItemRef(server: server, id: "../../etc/passwd")
		let path = AudioStore.path(root: root, key: CacheKeys.of(ref, quality: .original))
		#expect(!path.lastPathComponent.contains("/"))
		#expect(path.deletingLastPathComponent().lastPathComponent == server.description)
	}
}
