//	GainDrive for iOS
//	Copyright (C) 2026 Kasper Peeters
//
//	This file is part of GainDrive, distributed under the GNU General
//	Public License version 3 or later, with the Application Distribution
//	Exception in ios/LICENSE. See LICENSE at the repository root for the
//	full text of the GPL.

import Foundation

/// What was pinned: a track, an album, or a playlist.
///
/// Not `StarKind`, which is song/album/**artist** and answers a different
/// question. An artist cannot be pinned — a discography is not a unit anybody
/// means to download in one gesture — and a playlist cannot be starred.
enum PinKind: String, Codable, Hashable, Sendable {
	case song, album, playlist
}

/// One thing the user asked to keep.
///
/// **A pin records intent, not the songs it currently expands to.** A playlist
/// that gains a track should have the pin cover it, which it only can if the
/// pin is on the playlist. `android/CACHING.md` states the same rule, and it is
/// the whole reason this is not simply a list of song refs.
struct Pin: Codable, Hashable, Sendable, Identifiable {
	let ref: ItemRef
	let kind: PinKind
	/// What it was called when it was pinned, so the Storage screen can list
	/// pins without fetching every album to find out what they are — which
	/// offline, the one time that screen matters most, it could not do.
	let name: String

	/// The kind is part of the identity: an album and a track can hold the
	/// same id on servers that do not separate their id spaces, and the two
	/// are different pins.
	var id: String { "\(kind.rawValue):\(ref.encoded)" }
}

/// Turning pins plus resolved membership into the set of songs that must be
/// kept.
///
/// Pure and separate from `PinRepository` for the reason `QueueMove` and
/// `ScrobbleRule` are: it is the rule that decides what survives, and being a
/// free function is what lets it be tested without a store, a network or a
/// database.
enum Pins {
	/// `membership` maps a pin's id to the songs it was last resolved to.
	/// A song pin needs no entry — it is its own membership.
	static func expand(_ pins: [Pin], membership: [Pin.ID: [ItemRef]]) -> Set<ItemRef> {
		var wanted: Set<ItemRef> = []
		for pin in pins {
			switch pin.kind {
			case .song:
				wanted.insert(pin.ref)
			case .album, .playlist:
				wanted.formUnion(membership[pin.id] ?? [])
			}
		}
		return wanted
	}

	/// Folds a freshly resolved membership into the stored one.
	///
	/// **A pin that comes back empty keeps what it had.** An empty expansion is
	/// a legitimate *state* — a pinned album whose tracks have never been
	/// fetched protects nothing — but it is never a legitimate *transition* for
	/// a pin that already covered something. What produces one is a failed or
	/// cancelled read, and letting that through would drop songs the user
	/// explicitly asked to keep, silently, and only until they next happened to
	/// open the album.
	///
	/// Stale membership is the safer of the two wrong answers: it keeps bytes
	/// that are already on disk, and the next successful read replaces it.
	static func merging(
		_ existing: [Pin.ID: [ItemRef]], resolved: [Pin.ID: [ItemRef]]
	) -> [Pin.ID: [ItemRef]] {
		var merged = existing
		for (id, songs) in resolved where !songs.isEmpty {
			merged[id] = songs
		}
		return merged
	}

	/// Membership for pins that no longer exist is not kept: it would grow
	/// without bound and would protect songs nobody asked for.
	static func pruned(_ membership: [Pin.ID: [ItemRef]], to pins: [Pin]) -> [Pin.ID: [ItemRef]] {
		let live = Set(pins.map(\.id))
		return membership.filter { live.contains($0.key) }
	}
}
