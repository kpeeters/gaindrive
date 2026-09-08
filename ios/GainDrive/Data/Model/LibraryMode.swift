//	GainDrive for iOS
//	Copyright (C) 2026 Kasper Peeters
//
//	This file is part of GainDrive, distributed under the GNU General
//	Public License version 3 or later, with the Application Distribution
//	Exception in ios/LICENSE. See LICENSE at the repository root for the
//	full text of the GPL.

import Foundation

/// Which slice of the library the top-level list is showing.
///
/// **A wrapper over a plain string rather than an enum**, deliberately, and for
/// the reason `data/model/LibraryMode.kt` gives: the slices on offer come from
/// the server, so an enum would have to be edited every time a server grew a
/// new kind of root and would have no sensible case for one it had never heard
/// of.
///
/// A listing must never mix slices, which is why this is a single value rather
/// than a set: the server filters on it, so the result is single-slice by
/// construction.
///
/// The string names one of two things today:
///
/// * a **content type** — `artists`, `categories` — a gaindrive extension
///   naming a *kind* of root, which may span several of them
/// * **`uploads`**, the account's own upload area
///
/// Android has a third spelling, `folder:<name>`, for a server browsing by
/// folder. That arrives with folder browsing itself; the string is the reason
/// it will need no migration when it does, since `SettingsStore` stores this
/// value as it stands and an unrecognised one already falls back.
struct LibraryMode: Hashable, Sendable, Identifiable, Codable {
	let id: String

	init(_ id: String) {
		self.id = id
	}

	/// What a server naming no kind of root is taken to hold.
	static let artists = LibraryMode("artists")

	/// Sections rather than performers — Film, Series. A content type like any
	/// other; named here because the artist header keys on it: a section has no
	/// portrait and no biography, and the server refuses to look one up
	/// (`is_category_folder()`).
	static let categories = LibraryMode("categories")

	/// The account's own upload area.
	///
	/// **Looks like a content type and is not one.** The server deliberately
	/// keeps its uploads root out of `getMusicFolders`, so no amount of
	/// inspecting the roots will ever produce this chip — it is offered because
	/// the *account* may upload. It reaches the wire as `personal=true` rather
	/// than as a `contentType`, which is the whole of what `LibraryRoots` has
	/// to special-case.
	static let uploads = LibraryMode("uploads")

	/// Title-cased for display, falling back to capitalising the raw id so a
	/// kind this app has never seen still shows something rather than nothing.
	var label: String {
		if let known = Self.labels[id] { return known }
		guard let first = id.first else { return id }
		return first.uppercased() + id.dropFirst()
	}

	private static let labels = [
		"artists": "Artists",
		"categories": "Categories",
		"uploads": "Uploads",
	]
}
