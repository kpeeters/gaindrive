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

/// What a pin protects.
///
/// The rule that decides which audio survives, and every failure here is
/// silent: too little coverage deletes music somebody asked to keep, too much
/// keeps bytes nobody wants. Neither raises anything.
struct PinsTests {
	private let server = ServerId()

	private func ref(_ id: String) -> ItemRef {
		ItemRef(server: server, id: id)
	}

	private func pin(_ id: String, _ kind: PinKind) -> Pin {
		Pin(ref: ref(id), kind: kind, name: "Pin \(id)")
	}

	@Test func aTrackPinIsItsOwnMembership() {
		let one = pin("7", .song)
		#expect(Pins.expand([one], membership: [:]) == [ref("7")])
	}

	@Test func anAlbumPinCoversWhatItWasResolvedTo() {
		let album = pin("1", .album)
		let songs = [ref("10"), ref("11")]
		#expect(Pins.expand([album], membership: [album.id: songs]) == Set(songs))
	}

	/// The reason a pin records intent rather than a song list: the membership
	/// is replaced, and the pin goes on covering whatever it now holds.
	@Test func aPlaylistPinPicksUpATrackAddedSince() {
		let list = pin("2", .playlist)
		let before = [list.id: [ref("10")]]
		let after = Pins.merging(before, resolved: [list.id: [ref("10"), ref("99")]])
		#expect(Pins.expand([list], membership: after) == [ref("10"), ref("99")])
	}

	/// **The one that loses data when it is wrong.** An empty expansion is a
	/// legitimate state — a pinned album never read protects nothing — but
	/// never a legitimate transition for a pin that already covered something.
	/// A failed read must not be allowed to look like an emptied album.
	@Test func anEmptyResolutionKeepsWhatThePinAlreadyCovered() {
		let album = pin("1", .album)
		let before = [album.id: [ref("10"), ref("11")]]
		let after = Pins.merging(before, resolved: [album.id: []])
		#expect(after == before)
	}

	@Test func aPinThatIsGoneTakesItsMembershipWithIt() {
		let kept = pin("1", .album)
		let dropped = pin("2", .album)
		let membership = [kept.id: [ref("10")], dropped.id: [ref("20")]]
		#expect(Pins.pruned(membership, to: [kept]) == [kept.id: [ref("10")]])
	}

	/// An album and a playlist may hold the same track, and it is one file.
	@Test func twoPinsOverTheSameTrackWantItOnce() {
		let album = pin("1", .album)
		let list = pin("2", .playlist)
		let shared = ref("10")
		let wanted = Pins.expand(
			[album, list], membership: [album.id: [shared], list.id: [shared, ref("11")]])
		#expect(wanted == [shared, ref("11")])
	}

	/// The kind is part of the identity: on a server that does not separate
	/// its id spaces an album and a track can carry the same id, and the two
	/// are different pins.
	@Test func theSameIdUnderTwoKindsIsTwoPins() {
		#expect(pin("1", .album).id != pin("1", .song).id)
	}

	// MARK: - The size estimate

	private func song(_ id: String, seconds: Int, bytes: Int) -> Song {
		Song(
			ref: ref(id), title: "T", artistName: "A", albumTitle: "B", albumRef: nil,
			track: nil, discNumber: nil, year: nil, duration: seconds, bitRate: nil,
			suffix: nil, contentType: nil, sizeBytes: bytes, coverArt: nil, starredAt: nil,
			lastPlayedAt: nil, isVideo: false, nativeSeek: false, width: nil, height: nil)
	}

	/// **Not the stored size.** A five-minute FLAC is tens of megabytes and its
	/// AAC 160 transcode is about six, so estimating from `sizeBytes` would
	/// refuse a pin that fits several times over.
	@Test func theEstimateFollowsTheRequestedRateAndNotTheStoredSize() {
		let flac = song("1", seconds: 300, bytes: 40_000_000)
		let estimate = Pins.estimatedBytes(
			of: flac, quality: AudioQuality(format: .m4a, bitRate: 160))
		#expect(estimate == 300 * 160 * 1000 / 8)
		#expect(estimate < Int64(flac.sizeBytes))
	}

	/// Asking for the original really does mean the file as stored.
	@Test func theOriginalIsEstimatedAtItsRealSize() {
		let flac = song("1", seconds: 300, bytes: 40_000_000)
		#expect(Pins.estimatedBytes(of: flac, quality: .original) == 40_000_000)
	}

	/// A duration of zero is "not known", and arithmetic over it would say a
	/// track costs nothing at all.
	@Test func anUnknownDurationFallsBackToTheStoredSize() {
		let odd = song("1", seconds: 0, bytes: 1234)
		#expect(
			Pins.estimatedBytes(of: odd, quality: AudioQuality(format: .m4a, bitRate: 160))
				== 1234)
	}
}
