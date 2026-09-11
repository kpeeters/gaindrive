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

/// What one server is asked for the merged library list, and for uploads.
///
/// Every decision is a function of one server's `getMusicFolders` answer,
/// which is why it is exercised by calling it. The failure that matters most
/// is silent: asking a server for a kind it does not have gets its **entire**
/// library back, so the same folders appear in both groups and nothing
/// anywhere says why.
///
/// Android's counterpart additionally pins a folder-browsing server's
/// unnarrowed request; iOS has no folder-browse mode, so that case has no
/// counterpart here.
struct LibraryRootsTests {
	private func root(_ id: String, _ name: String, type: String? = nil) -> MusicRoot {
		MusicRoot(id: id, name: name, contentType: type)
	}

	private var typed: [MusicRoot] {
		[
			root("1", "music", type: "artists"),
			root("2", "more music", type: "artists"),
			root("3", "films", type: "categories"),
		]
	}

	// MARK: - The merged listing's requests

	/// A server naming both kinds is asked once per kind — and the *distinct*
	/// kinds, not once per root: two music roots are one artists request,
	/// because a kind may span several roots and the server filters on the
	/// kind.
	@Test func aServerNamingBothKindsIsAskedForEach() {
		let requests = LibraryRoots.listingRequests(roots: typed)
		#expect(requests.categories?.contentType == "categories")
		#expect(requests.artists?.contentType == "artists")
		#expect(requests.categories?.musicFolderId == nil)
		#expect(requests.artists?.musicFolderId == nil)
		#expect(requests.categories?.personal == PersonalScope.none)
		#expect(requests.artists?.personal == PersonalScope.none)
	}

	/// The group a server lacks is not asked for at all — asking would come
	/// back empty at best, and on a server predating roots as the entire
	/// library, putting the same folders in both groups.
	@Test func aServerWithoutCategoriesContributesNothingToThatGroup() {
		let requests = LibraryRoots.listingRequests(
			roots: [root("1", "music", type: "artists")])
		#expect(requests.categories == nil)
		#expect(requests.artists?.contentType == "artists")
	}

	@Test func aCategoriesOnlyServerContributesNothingToTheArtistsGroup() {
		let requests = LibraryRoots.listingRequests(
			roots: [root("1", "films", type: "categories")])
		#expect(requests.categories?.contentType == "categories")
		#expect(requests.artists == nil)
	}

	/// A kind this build has never heard of contributes nothing: the merged
	/// list has exactly two groups and nowhere meaningful to put it.
	@Test func anUnknownContentTypeContributesNothing() {
		let requests = LibraryRoots.listingRequests(
			roots: [root("1", "stuff", type: "podcasts")])
		#expect(requests.categories == nil)
		#expect(requests.artists == nil)
	}

	/// The guarantee for a server that has never heard of roots, pinned: it is
	/// still sent `contentType=artists`, which is exactly what this app sent
	/// before roots existed and which such a server ignores.
	@Test func anUntypedServerIsStillSentContentTypeArtists() {
		for roots in [[], [root("1", "Music"), root("2", "Podcasts")]] {
			let requests = LibraryRoots.listingRequests(roots: roots)
			#expect(requests.categories == nil)
			#expect(requests.artists?.contentType == "artists")
			#expect(requests.artists?.musicFolderId == nil)
		}
	}

	/// The shared library must go on sending exactly what it sent before.
	@Test func theSharedLibrarySendsNoPersonalParameter() {
		let requests = LibraryRoots.listingRequests(roots: typed)
		#expect(requests.artists?.personal.parameter == nil)
		#expect(requests.categories?.personal.parameter == nil)
	}

	// MARK: - The uploads listing
	//
	// A fact about the account, not about the roots: the server keeps its
	// uploads root out of getMusicFolders entirely, so uploads is its own
	// request rather than a half of listingRequests.

	/// `personal=true` and nothing else. Sending `contentType=uploads` would
	/// narrow the *shared* library to a kind no server has, and answer with an
	/// empty list rather than an error.
	@Test func uploadsIsAskedAsPersonalAndNarrowsNothingElse() {
		let request = LibraryRoots.uploadsRequest(isAdmin: false)
		#expect(request.personal == .mine)
		#expect(request.personal.parameter == "true")
		#expect(request.contentType == nil)
		#expect(request.musicFolderId == nil)
	}

	/// An admin gets everybody's, because an admin is the only account that
	/// can promote an upload into the shared library — without this a
	/// non-admin's upload is visible to its owner and to nobody able to act
	/// on it.
	@Test func anAdminAsksForEveryAccountsUploads() {
		let request = LibraryRoots.uploadsRequest(isAdmin: true)
		#expect(request.personal == .all)
		#expect(request.personal.parameter == "*")
		#expect(request.contentType == nil)
		#expect(request.musicFolderId == nil)
	}
}
