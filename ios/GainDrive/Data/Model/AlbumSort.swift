//	GainDrive for iOS
//	Copyright (C) 2026 Kasper Peeters
//
//	This file is part of GainDrive, distributed under the GNU General
//	Public License version 3 or later, with the Application Distribution
//	Exception in ios/LICENSE. See LICENSE at the repository root for the
//	full text of the GPL.

import Foundation

/// Which order an artist's albums are listed in.
///
/// `year` is what the server answers with — `get_artist()` in
/// `src/mediastore.cc` ends its query `ORDER BY al.year, al.title COLLATE
/// NOCASE` — and is the default here for that reason. It is right for a
/// discography and useless for a film category, where the only thing anyone
/// knows about an item is its name.
///
/// Stored by its raw value, like `AudioQuality`'s tag, so a value written by an
/// older build still parses and an unknown one falls back rather than throwing.
enum AlbumSort: String, CaseIterable, Sendable {
	case year
	case name

	static let `default` = AlbumSort.year

	var label: String {
		switch self {
		case .year: "Year"
		case .name: "Name"
		}
	}

	static func parse(_ raw: String?) -> AlbumSort {
		raw.flatMap(AlbumSort.init(rawValue:)) ?? .default
	}

	/// **A total order**, so the tie-break is never left to which server
	/// answered first.
	///
	/// The year arm reproduces the server's own `ORDER BY`: an album with no
	/// year sorts first, as it does under SQLite, where the column is NULL
	/// rather than zero. `caseInsensitiveCompare` stands in for `COLLATE
	/// NOCASE`, and is deliberately the *non*-localised comparison — a sort
	/// that changed with the phone's region would make a test that passes here
	/// fail there.
	///
	/// The final tie-break is the ref, which Android does not have and which is
	/// what makes this genuinely total: with duplicate merging switched off,
	/// the same record on two servers is two rows with the same title and year,
	/// and their order would otherwise depend on arrival.
	///
	/// Sorting is worth doing even under `year`, which is what the server
	/// already answered with: a merged artist's albums arrive as one server's
	/// list concatenated with another's — `Merge.albums` keeps arrival order —
	/// so the union was never in year order at all.
	func precedes(_ lhs: Album, _ rhs: Album) -> Bool {
		switch self {
		case .year:
			if lhs.sortYear != rhs.sortYear { return lhs.sortYear < rhs.sortYear }
			let titles = lhs.title.caseInsensitiveCompare(rhs.title)
			if titles != .orderedSame { return titles == .orderedAscending }
		case .name:
			let titles = lhs.title.caseInsensitiveCompare(rhs.title)
			if titles != .orderedSame { return titles == .orderedAscending }
			if lhs.sortYear != rhs.sortYear { return lhs.sortYear < rhs.sortYear }
		}
		return lhs.ref.encoded < rhs.ref.encoded
	}
}

extension Album {
	/// Nil sorts first, as NULL does in the server's own `ORDER BY`.
	fileprivate var sortYear: Int { year ?? 0 }
}
