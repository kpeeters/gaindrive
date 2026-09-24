//	GainDrive for iOS
//	Copyright (C) 2026 Kasper Peeters
//
//	This file is part of GainDrive, distributed under the GNU General
//	Public License version 3 or later, with the Application Distribution
//	Exception in ios/LICENSE. See LICENSE at the repository root for the
//	full text of the GPL.

import Foundation

/// Which part of the library a listing, a mirror file or a sort preference
/// belongs to, mirroring `data/model/LibrarySection.kt`.
///
/// **A closed enum where its predecessor (`LibraryMode`) was an open string
/// wrapper.** The openness existed for the chip row, whose chips came from the
/// server and so could name a kind this build had never heard of. With the
/// chips gone there is nothing left to draw for an unknown kind - the merged
/// list has exactly two groups, and uploads is its own screen - so a root
/// typed with something new simply contributes nothing until the app learns
/// what it means.
///
/// `rawValue` is the string on the wire (`contentType`), in the mirror's
/// `indexes-<section>` file names, and in the `album_sort` dictionary's keys.
enum LibrarySection: String, Hashable, Sendable, Codable, CaseIterable {
	/// Performers. Also where every root of a server that names no kind of
	/// root lands - see `LibraryRoots.listingRequests`.
	case artists

	/// Sections rather than performers - Film, Series. Named separately
	/// because downstream screens key on it: a section has no portrait and no
	/// biography, and the server refuses to look one up
	/// (`is_category_folder()`).
	case categories

	/// The account's own upload area. Not a kind of root at all: the server
	/// keeps its uploads root out of `getMusicFolders`, and the listing
	/// reaches the wire as `personal=true` rather than as a `contentType`.
	/// It is a section like the others below that line, which is why the
	/// mirror needed no key change - the file is simply `indexes-uploads`.
	case uploads

	var id: String { rawValue }

	var label: String {
		switch self {
		case .artists: "Artists"
		case .categories: "Categories"
		case .uploads: "Uploads"
		}
	}
}

/// The Library screen's one merged list: every category folder from every
/// `categories` root, then the usual artist index buckets.
///
/// `categories` is flat rather than bucketed - the whole group sits under a
/// single "Categories" header, since a library has a handful of sections, not
/// hundreds. `artists` keeps the A–Z buckets the alphabet rail scrubs.
struct LibraryListing: Hashable, Sendable {
	var categories: [Artist]
	var artists: [ArtistIndex]

	var isEmpty: Bool { categories.isEmpty && artists.isEmpty }
}
