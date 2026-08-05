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

struct BrowseScopeTests {
	/// The sentinel shares a field with a server id, so it has to be something
	/// a `ServerId` can never be.
	@Test func theSentinelIsNotAUUID() {
		#expect(UUID(uuidString: BrowseScope.allStored) == nil)
	}

	@Test func roundTripsThroughItsStoredForm() {
		let id = ServerId()
		#expect(
			BrowseScope.restored(from: BrowseScope.oneServer(id).stored, available: [id])
				== .oneServer(id))
		#expect(
			BrowseScope.restored(from: BrowseScope.allServers.stored, available: [id])
				== .allServers)
	}

	/// A choice whose server was removed falls back to all rather than leaving
	/// the library permanently empty with no hint why.
	@Test func aStoredServerThatNoLongerExistsFallsBack() {
		let gone = ServerId()
		#expect(
			BrowseScope.restored(from: gone.value.uuidString, available: [ServerId()])
				== .allServers)
	}

	/// Disabling a server has to behave the same way as removing it: `available`
	/// is the enabled list, so the fallback covers both.
	@Test func nothingStoredMeansAllServers() {
		#expect(BrowseScope.restored(from: nil, available: [ServerId()]) == .allServers)
		#expect(BrowseScope.restored(from: "", available: [ServerId()]) == .allServers)
		#expect(BrowseScope.restored(from: "nonsense", available: [ServerId()]) == .allServers)
	}
}
