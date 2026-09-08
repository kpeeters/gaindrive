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

/// Where a stored answer lives.
///
/// The failure this guards is quiet in both directions: two answers sharing a
/// key means one library shown under another's chip, and a key that does not
/// reproduce means a mirror that never hits and a fallback that never fires.
struct MirrorKeyTests {
	private let server = ServerId()

	/// **A listing must never mix slices.** Categories stored over Artists
	/// would be a chip showing the wrong library the moment its server went
	/// away — which is precisely when nobody can check.
	@Test func eachSliceHasItsOwnIndex() {
		let artists = LibraryMirror.Key.indexes(server, .artists).name
		let categories = LibraryMirror.Key.indexes(server, .categories).name
		#expect(artists != categories)
	}

	/// An artist's album list and an album's track list are different answers
	/// and may hold the same id on servers that do not separate their id
	/// spaces.
	@Test func anAlbumListAndAnAlbumDoNotCollide() {
		let ref = ItemRef(server: server, id: "12")
		#expect(LibraryMirror.Key.albums(ref).name != LibraryMirror.Key.album(ref).name)
	}

	@Test func everyKeyBelongsToItsServer() {
		let ref = ItemRef(server: server, id: "12")
		#expect(LibraryMirror.Key.albums(ref).server == server)
		#expect(LibraryMirror.Key.chips(server).server == server)
		#expect(LibraryMirror.Key.indexes(server, .artists).server == server)
	}

	/// A Subsonic id is a string somebody else chose; one with a slash in it
	/// would otherwise write outside the directory it was meant for.
	@Test func anIdWithASlashStaysOneComponent() {
		let ref = ItemRef(server: server, id: "../escape")
		#expect(!LibraryMirror.Key.album(ref).name.contains("/"))
	}
}

/// That the mirror can actually store what it claims to.
///
/// The models gained `Codable` for this, and a conformance that silently
/// dropped a field would show up as a stored album with no tracks — which
/// reads as an empty album rather than as a broken cache.
struct MirroredValueTests {
	private let server = ServerId()

	private func song(_ id: String) -> Song {
		Song(
			ref: ItemRef(server: server, id: id), title: "Track \(id)", artistName: "A",
			albumTitle: "B", albumRef: nil, track: 3, discNumber: 1, year: 1979,
			duration: 210, bitRate: 160, suffix: "m4a", contentType: "audio/mp4",
			sizeBytes: 4_200_000, coverArt: nil, starredAt: nil, lastPlayedAt: nil,
			isVideo: false, nativeSeek: true, width: nil, height: nil)
	}

	@Test func anAlbumSurvivesTheRoundTrip() throws {
		let detail = AlbumDetail(
			album: Album(
				ref: ItemRef(server: server, id: "1"), title: "Unknown Pleasures",
				artistName: "Joy Division", songCount: 2, videoCount: 0, duration: 420,
				year: 1979),
			songs: [song("10"), song("11")])
		let data = try JSONEncoder().encode(detail)
		#expect(try JSONDecoder().decode(AlbumDetail.self, from: data) == detail)
	}

	/// A merged row stands for the same artist on several servers, and losing
	/// `refs` would quietly make it stand for one.
	@Test func aMergedArtistKeepsEveryServerItCameFrom() throws {
		let other = ServerId()
		let artist = Artist(
			ref: ItemRef(server: server, id: "1"), name: "Bowie", albumCount: 9,
			refs: [ItemRef(server: server, id: "1"), ItemRef(server: other, id: "7")])
		let data = try JSONEncoder().encode([ArtistIndex(label: "B", artists: [artist])])
		let back = try JSONDecoder().decode([ArtistIndex].self, from: data)
		#expect(back.first?.artists.first?.refs.count == 2)
		#expect(back.first?.artists.first == artist)
	}
}

/// How a fallback says what happened.
struct StoredFailureTests {
	/// **Reworded, not suppressed.** Carried in the message rather than as a
	/// flag, so every screen that already draws a partial-failure note says it
	/// without any of them learning what a mirror is.
	@Test func aRecoveredFailureStillNamesTheProblem() {
		let failure = ServerFailure(
			server: ServerId(), serverName: "home", message: "Could not reach the server.")
		let stored = failure.showingStored
		#expect(stored.message.hasPrefix("Could not reach the server."))
		#expect(stored.message.hasSuffix("Showing what was stored."))
		#expect(stored.server == failure.server)
		#expect(stored.serverName == failure.serverName)
	}
}
