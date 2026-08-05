//	GainDrive for iOS
//	Copyright (C) 2026 Kasper Peeters
//
//	This file is part of GainDrive, distributed under the GNU General
//	Public License version 3 or later, with the Application Distribution
//	Exception in ios/LICENSE. See LICENSE at the repository root for the
//	full text of the GPL.

import Foundation

/// A domain item with its cover already resolved, mirroring `ui/UiModels.kt`.
///
/// Rows stay dumb this way: a row is handed everything it draws and never asks
/// anyone for anything. Shared between browse and search so the two cannot
/// drift.
///
/// `cover` is a `CoverSource` rather than Android's plain URL string, because
/// the cache key is not derivable from the URL — see `CoverUrls`.
struct AlbumUi: Identifiable, Hashable, Sendable {
	let album: Album
	let cover: CoverSource?
	/// **Plural.** A row whose duplicates were collapsed stands for every
	/// server that has the album, and hiding that would make the missing second
	/// row look like a bug.
	let badges: [String]

	init(album: Album, cover: CoverSource? = nil, badges: [String] = []) {
		self.album = album
		self.cover = cover
		self.badges = badges
	}

	var id: ItemRef { album.ref }
}

struct ArtistUi: Identifiable, Hashable, Sendable {
	let artist: Artist
	let cover: CoverSource?
	let badges: [String]

	init(artist: Artist, cover: CoverSource? = nil, badges: [String] = []) {
		self.artist = artist
		self.cover = cover
		self.badges = badges
	}

	var id: ItemRef { artist.ref }
}

/// Singular `badge`, unlike the album and artist rows: songs never merge, so a
/// track row can only ever come from one server.
struct SongUi: Identifiable, Hashable, Sendable {
	let song: Song
	let cover: CoverSource?
	let badge: String?

	init(song: Song, cover: CoverSource? = nil, badge: String? = nil) {
		self.song = song
		self.cover = cover
		self.badge = badge
	}

	var id: ItemRef { song.ref }
}

/// Cover sizes, requested per context rather than per caller's guess.
///
/// The size is part of the cache key, so a row that asks for 144 and a hero
/// that asks for 800 are two entries — and a row that asked for a different
/// size each time would be a cache that never hits.
enum CoverSize {
	static let thumb = 144
	static let portrait = 288
	static let hero = 800
}
