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

/// The merge rules. All of this is invisible with one server configured, which
/// is exactly why it is tested.
struct MergeTests {
	private let serverA = ServerId()
	private let serverB = ServerId()

	private func artist(_ server: ServerId, _ id: String, _ name: String, albums: Int = 1)
		-> Artist
	{
		Artist(ref: ItemRef(server: server, id: id), name: name, albumCount: albums)
	}

	private func album(_ server: ServerId, _ id: String, _ title: String, artist: String = "Pink Floyd")
		-> Album
	{
		Album(ref: ItemRef(server: server, id: id), title: title, artistName: artist)
	}

	// MARK: - Artists

	@Test func collapsesArtistsOnCaseAndWhitespace() {
		let merged = Merge.artists(perServer: [
			[artist(serverA, "1", "Pink Floyd", albums: 3)],
			[artist(serverB, "9", "  pink floyd ", albums: 2)],
		])
		#expect(merged.count == 1)
		#expect(merged[0].albumCount == 5)
		#expect(merged[0].refs.count == 2)
		// The first contributor in registry order wins the ref and the name.
		#expect(merged[0].ref.server == serverA)
		#expect(merged[0].name == "Pink Floyd")
	}

	/// Starred anywhere is starred — the alternative is a star that depends on
	/// which server happened to answer first.
	@Test func starredAnywhereWins() {
		let plain = artist(serverA, "1", "Pink Floyd")
		let starred = Artist(
			ref: ItemRef(server: serverB, id: "9"), name: "Pink Floyd", albumCount: 1,
			starredAt: "2026-01-01T00:00:00Z")
		#expect(Merge.artists(perServer: [[plain], [starred]])[0].isStarred)
	}

	/// One server is not a merge. Returning it untouched also preserves the
	/// order it chose, which for search results is relevance.
	@Test func oneServerPassesThroughUntouched() {
		let list = [artist(serverA, "2", "Zappa"), artist(serverA, "1", "ABBA")]
		#expect(Merge.artists(perServer: [list]).map(\.name) == ["Zappa", "ABBA"])
	}

	/// Two servers filing the same artist under different letters must produce
	/// **one** row, in one bucket — otherwise merging has achieved nothing on
	/// exactly the screen it exists for.
	@Test func anArtistFiledUnderTwoLettersMergesIntoOne() {
		let merged = Merge.artistIndexes(perServer: [
			[ArtistIndex(label: "P", artists: [artist(serverA, "1", "Pink Floyd")])],
			[ArtistIndex(label: "F", artists: [artist(serverB, "9", "pink floyd")])],
		])
		#expect(merged.flatMap(\.artists).count == 1)
		#expect(merged.count == 1)
		#expect(merged[0].label == "P")
	}

	/// `#` belongs at the end of the rail, not where its code point puts it —
	/// which is before "A", and reads as a mistake.
	@Test func hashSortsLast() {
		let merged = Merge.artistIndexes(perServer: [
			[
				ArtistIndex(label: "#", artists: [artist(serverA, "1", "3 Doors")]),
				ArtistIndex(label: "B", artists: [artist(serverA, "2", "Blur")]),
			],
			[ArtistIndex(label: "A", artists: [artist(serverB, "3", "ABBA")])],
		])
		#expect(merged.map(\.label) == ["A", "B", "#"])
	}

	// MARK: - Categories

	/// The category half of the merged library list: every server's buckets
	/// flattened into one alphabetical group under a single header.
	@Test func categoriesFlattenAcrossBucketsAndSortAlphabetically() {
		let merged = Merge.categories(perServer: [
			[
				ArtistIndex(label: "F", artists: [artist(serverA, "1", "Film")]),
				ArtistIndex(label: "S", artists: [artist(serverA, "2", "Series")]),
			]
		])
		#expect(merged.map(\.name) == ["Film", "Series"])
	}

	/// The explicit sort is load-bearing: `Merge.artistIndexes` short-circuits
	/// a single server and returns its own bucket order, which under one
	/// header would read as no order at all.
	@Test func aSingleServersCategoriesStillComeOutAlphabetical() {
		let merged = Merge.categories(perServer: [
			[
				ArtistIndex(label: "S", artists: [artist(serverA, "2", "Series")]),
				ArtistIndex(label: "D", artists: [artist(serverA, "3", "Documentary")]),
				ArtistIndex(label: "F", artists: [artist(serverA, "1", "Film")]),
			]
		])
		#expect(merged.map(\.name) == ["Documentary", "Film", "Series"])
	}

	/// The same section on two servers is one row carrying both refs, the
	/// first contributor in registry order winning the spelling and the ref.
	@Test func sameNamedCategoriesCollapseAcrossServers() {
		let merged = Merge.categories(perServer: [
			[ArtistIndex(label: "F", artists: [artist(serverA, "1", "Film")])],
			[ArtistIndex(label: "F", artists: [artist(serverB, "9", "film")])],
		])
		#expect(merged.count == 1)
		#expect(merged[0].name == "Film")
		#expect(merged[0].refs.count == 2)
		#expect(merged[0].ref == ItemRef(server: serverA, id: "1"))
	}

	// MARK: - Albums

	@Test func collapsesAlbumsAcrossPunctuation() {
		let merged = Merge.albums([
			album(serverA, "1", "Ummagumma, Vol. 2"),
			album(serverB, "9", "Ummagumma Vol 2"),
		])
		#expect(merged.count == 1)
		#expect(merged[0].refs.count == 2)
		#expect(merged[0].sources == [serverA, serverB])
	}

	/// Two same-titled albums on **one** server are two albums — separately
	/// filed editions — and collapsing them would hide one.
	@Test func doesNotCollapseWithinOneServer() {
		let merged = Merge.albums([
			album(serverA, "1", "Live"),
			album(serverA, "2", "Live"),
		])
		#expect(merged.count == 2)
	}

	/// Even with a second server present, the same-server pair stays two rows.
	@Test func keepsSameServerEditionsWhileMergingAcrossServers() {
		let merged = Merge.albums([
			album(serverA, "1", "Live"),
			album(serverA, "2", "Live"),
			album(serverB, "9", "Live"),
		])
		#expect(merged.count == 2)
	}

	/// A different artist with the same album title is a different album.
	@Test func doesNotCollapseAcrossArtists() {
		let merged = Merge.albums([
			album(serverA, "1", "Greatest Hits", artist: "Queen"),
			album(serverB, "9", "Greatest Hits", artist: "ABBA"),
		])
		#expect(merged.count == 2)
	}

	/// A title made entirely of punctuation keeps its raw form, or every such
	/// title would collapse into one row.
	@Test func punctuationOnlyTitlesDoNotAllCollapse() {
		let merged = Merge.albums([
			album(serverA, "1", "!!!"),
			album(serverB, "9", "???"),
		])
		#expect(merged.count == 2)
	}

	@Test func matchKeyKeepsLettersAndDigitsOnly() {
		#expect(Merge.matchKey("Vol. 2") == Merge.matchKey("vol 2"))
		#expect(Merge.matchKey("Don’t") == Merge.matchKey("Don't"))
		#expect(Merge.matchKey("  A  B ") == "ab")
	}
}
