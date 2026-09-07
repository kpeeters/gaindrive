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

/// What the chip row offers, and what a chosen chip becomes on the wire.
///
/// Every decision is a function of one server's `getMusicFolders` answer and of
/// whether this account may upload to it, which is why it is exercised by
/// calling it. The failure that matters most is silent: asking a server for a
/// chip it does not have gets its **entire** library back, so the same artists
/// appear under every chip and nothing anywhere says why.
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

	// MARK: - Chips

	/// The *distinct* kinds, not one per root: a kind may span several roots
	/// and the server filters on the kind, so two music roots are one chip.
	@Test func chipsAreTheDistinctContentTypes() {
		#expect(
			LibraryRoots.chips(roots: typed, canUpload: false) == [.artists, .categories])
	}

	@Test func aServerNamingNoKindOffersArtistsAlone() {
		let untyped = [root("1", "music")]
		#expect(LibraryRoots.chips(roots: untyped, canUpload: false) == [.artists])
		#expect(LibraryRoots.chips(roots: [], canUpload: false) == [.artists])
	}

	/// A fact about the **account**, not the roots: the server keeps its
	/// uploads root out of `getMusicFolders` entirely, so no amount of reading
	/// them would produce this chip.
	@Test func uploadsComesFromTheAccountAndIsAppendedLast() {
		#expect(
			LibraryRoots.chips(roots: typed, canUpload: true)
				== [.artists, .categories, .uploads])
		#expect(LibraryRoots.chips(roots: [], canUpload: true) == [.artists, .uploads])
	}

	// MARK: - Requests

	/// **The one that must not regress.** A chip contributed by a different
	/// server is not a failure, and asking anyway is what would put the same
	/// artists under every chip.
	@Test func aChipThisServerCannotAnswerForIsNotAsked() {
		#expect(
			LibraryRoots.request(
				roots: [root("1", "music", type: "artists")], mode: .categories,
				canUpload: false, isAdmin: false) == nil)
		#expect(
			LibraryRoots.request(
				roots: typed, mode: .uploads, canUpload: false, isAdmin: false) == nil)
	}

	@Test func aContentTypeChipSendsContentType() {
		#expect(
			LibraryRoots.request(
				roots: typed, mode: .categories, canUpload: false, isAdmin: false)
				== RootRequest(contentType: "categories"))
	}

	/// Uploads is handled before everything else because it is **not a root**:
	/// `contentType=uploads` would narrow the shared library to a kind no
	/// server has and answer with nothing at all.
	@Test func uploadsSendsPersonalAndNoContentType() {
		let request = LibraryRoots.request(
			roots: typed, mode: .uploads, canUpload: true, isAdmin: false)
		#expect(request == RootRequest(personal: .mine))
		#expect(request?.contentType == nil)
		#expect(request?.personal.parameter == "true")
	}

	/// An admin is the only account that can promote an upload into the shared
	/// library, so without the wider scope a non-admin's upload is visible to
	/// its owner and to nobody able to act on it.
	@Test func anAdminSeesEverybodysUploads() {
		let request = LibraryRoots.request(
			roots: typed, mode: .uploads, canUpload: true, isAdmin: true)
		#expect(request?.personal == PersonalScope.all)
		#expect(request?.personal.parameter == "*")
	}

	/// Nil rather than `"false"`: the parameter would work, but appending one
	/// to every ordinary library request that never carried it is a gratuitous
	/// difference from what a third-party server has always seen.
	@Test func theSharedLibrarySendsNoPersonalParameter() {
		#expect(PersonalScope.none.parameter == nil)
	}

	// MARK: - Merging

	@Test func chipsFromSeveralServersDeduplicateCaseInsensitively() {
		let merged = LibraryRoots.mergeChips(perServer: [
			[.artists, .categories],
			[LibraryMode("Artists"), LibraryMode("podcasts")],
		])
		#expect(merged == [.artists, .categories, LibraryMode("podcasts")])
	}

	/// Ranked last explicitly rather than left to sort alphabetically among the
	/// content types, where it lands after "Categories" today and would land
	/// before a server's "Videos" tomorrow.
	@Test func uploadsSortsLastWhateverItIsSpelledNextTo() {
		let merged = LibraryRoots.mergeChips(perServer: [
			[.uploads, LibraryMode("videos")],
			[.artists],
		])
		#expect(merged == [.artists, LibraryMode("videos"), .uploads])
	}

	@Test func anUnknownKindStillGetsALabel() {
		#expect(LibraryMode("podcasts").label == "Podcasts")
		#expect(LibraryMode.uploads.label == "Uploads")
	}
}
