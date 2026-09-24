//	GainDrive for iOS
//	Copyright (C) 2026 Kasper Peeters
//
//	This file is part of GainDrive, distributed under the GNU General
//	Public License version 3 or later, with the Application Distribution
//	Exception in ios/LICENSE. See LICENSE at the repository root for the
//	full text of the GPL.

import Foundation

//	What one server is asked for the merged library list, and for the uploads
//	listing.
//
//	**Pure**, mirroring `data/browse/LibraryRoots.kt`: every decision here is a
//	function of one server's `getMusicFolders` answer, so it is exercised by
//	calling it rather than through a repository that would need a server to
//	run at all.

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
	/// because a bucket label was always just a string - but it is why the
	/// alphabet rail has to be suppressed when the labels are not letters.
	case all

	/// What goes on the wire, or nil to send no parameter at all.
	///
	/// `none` is deliberately nil rather than `"false"`. The server tests for
	/// the exact strings, so `"false"` would work - but it would also append a
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
/// `personal` is not a third way of naming a root - it switches to a different
/// library altogether, and the server ignores the other two while it is set.
/// Kept in one value regardless, because every caller wants exactly one of
/// these and separate parameters could be passed inconsistently.
struct RootRequest: Hashable, Sendable {
	var musicFolderId: String?
	var contentType: String?
	var personal: PersonalScope = .none
}

/// The one or two requests one server's merged listing is built from. A nil
/// half is a group this server lacks - it contributes nothing there, which is
/// not a failure and must not be reported as one. Not asking is the point: a
/// request for a kind a server does not have would either come back empty or,
/// on a server predating library roots, come back as the **entire** library
/// and put the same folders in both groups. Filtering the reply instead would
/// be too late: nothing in it says which rows to discard.
struct ListingRequests: Hashable, Sendable {
	let categories: RootRequest?
	let artists: RootRequest?
}

enum LibraryRoots {
	/// What to ask `roots`' server for the merged list.
	///
	/// A typed server - any root naming a `contentType` - is asked per kind it
	/// actually has, because omitting the parameter there answers with every
	/// root mixed together, which is exactly what the two-group list exists to
	/// avoid. A root typed with a kind this build has never heard of
	/// contributes nothing; the old chip row could surface an unknown kind as
	/// a chip of its own, but the merged list has nowhere meaningful to put
	/// one.
	///
	/// An untyped server has no opinion about what its roots contain, so all
	/// of them land in the artists group. It keeps being sent
	/// `contentType=artists`, which is exactly what this app sent before roots
	/// existed and which a server that has never heard of it ignores. Android
	/// distinguishes a folder-browsing server here and sends nothing at all;
	/// iOS has no folder-browse mode, so there is no such case - the seam for
	/// it is `RootRequest.musicFolderId`, which nothing sends yet.
	static func listingRequests(roots: [MusicRoot]) -> ListingRequests {
		let types = Set(roots.compactMap(\.contentType))

		let categories = types.contains(LibrarySection.categories.id)
			? RootRequest(contentType: LibrarySection.categories.id) : nil

		let artists: RootRequest? =
			if types.isEmpty || types.contains(LibrarySection.artists.id) {
				RootRequest(contentType: LibrarySection.artists.id)
			} else {
				nil
			}

		return ListingRequests(categories: categories, artists: artists)
	}

	/// The uploads listing. Apart from `listingRequests` because it is not a
	/// root: sending `contentType=uploads` would narrow the *shared* library
	/// to a kind no server has, and answer with nothing at all.
	///
	/// `isAdmin` widens the listing to every account's. An admin is the only
	/// account that can promote an upload into the shared library, so without
	/// the wider scope a non-admin's upload is visible to its owner and to
	/// nobody able to act on it.
	static func uploadsRequest(isAdmin: Bool) -> RootRequest {
		RootRequest(personal: isAdmin ? .all : .mine)
	}
}
