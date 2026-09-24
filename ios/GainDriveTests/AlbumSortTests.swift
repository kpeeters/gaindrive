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

struct AlbumSortTests {
	private let server = ServerId()

	private func album(_ id: String, _ title: String, year: Int? = nil, on: ServerId? = nil)
		-> Album
	{
		Album(
			ref: ItemRef(server: on ?? server, id: id), title: title, artistName: "A",
			year: year)
	}

	private func order(_ sort: AlbumSort, _ albums: [Album]) -> [String] {
		albums.sorted { sort.precedes($0, $1) }.map(\.title)
	}

	/// Reproduces the server's own `ORDER BY al.year, al.title`: an album with
	/// no year sorts first, as it does under SQLite where the column is NULL
	/// rather than zero.
	@Test func yearOrderPutsAnUndatedAlbumFirst() {
		let albums = [album("1", "Later", year: 1999), album("2", "Undated")]
		#expect(order(.year, albums) == ["Undated", "Later"])
	}

	@Test func yearTiesBreakOnTitleCaseInsensitively() {
		let albums = [
			album("1", "beta", year: 1980),
			album("2", "Alpha", year: 1980),
		]
		#expect(order(.year, albums) == ["Alpha", "beta"])
	}

	/// The reason the control exists: year order is right for a discography and
	/// useless for a film category, where the only thing anyone knows about an
	/// item is its name.
	@Test func nameOrderIgnoresTheYear() {
		let albums = [
			album("1", "Zulu", year: 1964),
			album("2", "Alien", year: 1979),
		]
		#expect(order(.name, albums) == ["Alien", "Zulu"])
	}

	/// **The property that makes it a total order.** With duplicate merging
	/// switched off the same record on two servers is two rows with the same
	/// title and year, and their order would otherwise depend on which server
	/// answered first. The ref decides it - which one wins is arbitrary, that
	/// it is always the same one is not.
	@Test func twoIdenticalAlbumsOnDifferentServersHaveAStableOrder() {
		let other = ServerId()
		let mine = album("1", "Kind of Blue", year: 1959)
		let theirs = album("1", "Kind of Blue", year: 1959, on: other)
		for sort in AlbumSort.allCases {
			#expect(sort.precedes(mine, theirs) != sort.precedes(theirs, mine))
		}
	}

	@Test func anAlbumDoesNotPrecedeItself() {
		let only = album("1", "One", year: 1970)
		for sort in AlbumSort.allCases {
			#expect(!sort.precedes(only, only))
		}
	}

	/// Stored by raw value, so a value written by a build that knew a sort this
	/// one does not still parses rather than throwing.
	@Test func anUnknownStoredValueFallsBackToYear() {
		#expect(AlbumSort.parse("name") == .name)
		#expect(AlbumSort.parse("shuffle") == .year)
		#expect(AlbumSort.parse(nil) == .year)
	}
}
