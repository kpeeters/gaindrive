//	GainDrive for iOS
//	Copyright (C) 2026 Kasper Peeters
//
//	This file is part of GainDrive, distributed under the GNU General
//	Public License version 3 or later, with the Application Distribution
//	Exception in ios/LICENSE. See LICENSE at the repository root for the
//	full text of the GPL.

import Foundation

//	What the chip row offers, and what a chosen chip becomes on the wire.
//
//	**Pure**, mirroring `data/browse/LibraryRoots.kt`: every decision here is a
//	function of one server's `getMusicFolders` answer and of whether this
//	account may upload to it, so it is exercised by calling it rather than
//	through a repository that would need a server to run at all.

/// Whose uploads a listing covers, if anyone's.
///
/// An enum rather than two booleans: exactly one of these is true at a time,
/// and a pair could be set to a combination that means nothing.
enum PersonalScope: Hashable, Sendable {
	/// The shared library.
	case none
	/// This account's own uploads.
	case mine
	/// Every account's uploads, which the server allows only for an admin.
	///
	/// It also groups the response **by owner instead of by first letter**, so
	/// the index labels come back as usernames. That needs no client change,
	/// because a bucket label was always just a string — but it is why the
	/// alphabet rail has to be suppressed when the labels are not letters.
	case all

	/// What goes on the wire, or nil to send no parameter at all.
	///
	/// `none` is deliberately nil rather than `"false"`. The server tests for
	/// the exact strings, so `"false"` would work — but it would also append a
	/// parameter to every ordinary library request that never carried one,
	/// which is a gratuitous difference from what a third-party server has
	/// always seen.
	var parameter: String? {
		switch self {
		case .none: nil
		case .mine: "true"
		case .all: "*"
		}
	}
}

/// How a top-level listing is narrowed. Everything at its default means "the
/// whole shared library".
///
/// `personal` is not a third way of naming a root — it switches to a different
/// library altogether, and the server ignores the other two while it is set.
/// Kept in one value regardless, because every caller wants exactly one of
/// these and separate parameters could be passed inconsistently.
struct RootRequest: Hashable, Sendable {
	var musicFolderId: String?
	var contentType: String?
	var personal: PersonalScope = .none
}

enum LibraryRoots {
	/// The chips one server contributes.
	///
	/// Two cases today, tested in this order:
	///
	/// 1. **Any root names a content type** — the server understands kinds of
	///    root, so the chips are those kinds. Note they are the *distinct*
	///    values and not one per root: two artist roots are one "Artists" chip,
	///    because a kind may span several roots and the server filters on the
	///    kind.
	/// 2. **Anything else** — one Artists chip, which with a single server
	///    means no chip row is drawn at all.
	///
	/// Android has a third case between them, one chip per untyped root for a
	/// server browsing by folder. It arrives with folder browsing.
	///
	/// `canUpload` appends the Uploads chip to whichever applies, and is a fact
	/// about the **account** rather than about the roots — the server keeps its
	/// uploads root out of `getMusicFolders` entirely, so no amount of reading
	/// `roots` would produce it. It is appended last in both cases, and it is
	/// the one chip allowed to turn a would-be single-chip row into a real one:
	/// a row of one chip is normally a control that does nothing, but Uploads
	/// is somewhere else to be.
	static func chips(roots: [MusicRoot], canUpload: Bool) -> [LibraryMode] {
		let uploads = canUpload ? [LibraryMode.uploads] : []
		let types = orderedDistinct(roots.compactMap(\.contentType))
		guard types.isEmpty else { return types.map(LibraryMode.init) + uploads }
		return [.artists] + uploads
	}

	/// What to send this server for `mode`, or **nil when it cannot answer for
	/// it at all** — a chip contributed by a different server, which is not a
	/// failure and must not be reported as one.
	///
	/// Not asking is the point. A server predating library roots ignores an
	/// unknown parameter and answers with its **entire** library, so the
	/// request itself is what would put the same artists under every chip.
	/// Filtering the reply instead would be too late: nothing in it says which
	/// rows to discard.
	static func request(
		roots: [MusicRoot], mode: LibraryMode, canUpload: Bool, isAdmin: Bool
	) -> RootRequest? {
		guard chips(roots: roots, canUpload: canUpload).contains(mode) else { return nil }

		// Before everything below, because it is not a root: sending
		// `contentType=uploads` would narrow the *shared* library to a kind no
		// server has, and answer with nothing at all.
		if mode == .uploads {
			// An admin is the only account that can promote an upload into the
			// shared library, so without the wider scope a non-admin's upload
			// is visible to its owner and to nobody able to act on it.
			return RootRequest(personal: isAdmin ? .all : .mine)
		}

		// A content-type chip on a server naming no types is the fallback
		// Artists chip. It keeps sending `contentType=artists`, which is
		// exactly what this app sent before roots existed and which a server
		// that has never heard of it ignores.
		return RootRequest(contentType: mode.id)
	}

	/// The chips of every server in scope, as one row.
	///
	/// Content types first, then Uploads, each group alphabetically — so a row
	/// mixing chips from different servers has a stable shape rather than one
	/// that depends on which server answered first. Deduplicated
	/// case-insensitively, keeping the first spelling in registry order, which
	/// is the tie-break every other merge uses.
	///
	/// **Uploads is ranked last explicitly** rather than left to sort
	/// alphabetically among the content types, where it happens to land after
	/// "Artists" and "Categories" today and would land before a server's
	/// "Videos" tomorrow. It is not one of the library's slices — it is the
	/// user's own corner of the server — so it belongs at the end whatever it
	/// is spelled next to.
	static func mergeChips(perServer: [[LibraryMode]]) -> [LibraryMode] {
		var seen: [String: LibraryMode] = [:]
		var order: [String] = []
		for mode in perServer.flatMap({ $0 }) {
			let key = mode.id.lowercased()
			guard seen[key] == nil else { continue }
			seen[key] = mode
			order.append(key)
		}
		return order.compactMap { seen[$0] }.sorted { lhs, rhs in
			let ranks = (rank(lhs), rank(rhs))
			if ranks.0 != ranks.1 { return ranks.0 < ranks.1 }
			let labels = lhs.label.caseInsensitiveCompare(rhs.label)
			if labels != .orderedSame { return labels == .orderedAscending }
			return lhs.id < rhs.id
		}
	}

	private static func rank(_ mode: LibraryMode) -> Int {
		mode == .uploads ? 1 : 0
	}

	/// Distinct, in first-seen order. `Set` would lose the order, and the order
	/// is what makes a single server's row match the order its roots are
	/// configured in.
	private static func orderedDistinct(_ values: [String]) -> [String] {
		var seen: Set<String> = []
		return values.filter { seen.insert($0).inserted }
	}
}
