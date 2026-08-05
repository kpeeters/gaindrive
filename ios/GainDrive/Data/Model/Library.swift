//	GainDrive for iOS
//	Copyright (C) 2026 Kasper Peeters
//
//	This file is part of GainDrive, distributed under the GNU General
//	Public License version 3 or later, with the Application Distribution
//	Exception in ios/LICENSE. See LICENSE at the repository root for the
//	full text of the GPL.

import Foundation

//	The domain models, mirroring `data/model/Library.kt`.
//
//	Separate from the DTOs on purpose. It is tempting to skip the mapping layer
//	at this size, but phase 5's offline mirror needs models that can come from
//	either the network or the local database, and retrofitting that split later
//	would touch every screen.
//
//	Every identifier here is an `ItemRef`. Nothing in this file holds a bare
//	`String` id, and nothing above it should either.

/// An artist, possibly standing for the same artist on several servers.
struct Artist: Identifiable, Hashable, Sendable {
	let ref: ItemRef
	let name: String
	let albumCount: Int
	let coverArt: ItemRef?
	let starredAt: String?
	/// **Ids, not just server ids.** Opening a merged artist has to ask each
	/// contributing server for *its* artist, and those ids differ. A row that
	/// remembered only which servers it came from could not do that.
	let refs: [ItemRef]

	init(
		ref: ItemRef, name: String, albumCount: Int, coverArt: ItemRef? = nil,
		starredAt: String? = nil, refs: [ItemRef]? = nil
	) {
		self.ref = ref
		self.name = name
		self.albumCount = albumCount
		self.coverArt = coverArt
		self.starredAt = starredAt
		self.refs = refs ?? [ref]
	}

	var id: ItemRef { ref }
	var isStarred: Bool { starredAt != nil }
	var sources: [ServerId] { refs.map(\.server) }
}

/// One bucket of the index rail: a letter and the artists filed under it.
struct ArtistIndex: Identifiable, Hashable, Sendable {
	let label: String
	let artists: [Artist]

	var id: String { label }
}

struct Album: Identifiable, Hashable, Sendable {
	let ref: ItemRef
	let title: String
	let artistName: String
	let artistRef: ItemRef?
	let songCount: Int
	let duration: Int
	let year: Int?
	let genre: String?
	let coverArt: ItemRef?
	let starredAt: String?
	/// As `Artist.refs` — a merged album row stands for the same record on
	/// several servers.
	let refs: [ItemRef]

	init(
		ref: ItemRef, title: String, artistName: String, artistRef: ItemRef? = nil,
		songCount: Int = 0, duration: Int = 0, year: Int? = nil, genre: String? = nil,
		coverArt: ItemRef? = nil, starredAt: String? = nil, refs: [ItemRef]? = nil
	) {
		self.ref = ref
		self.title = title
		self.artistName = artistName
		self.artistRef = artistRef
		self.songCount = songCount
		self.duration = duration
		self.year = year
		self.genre = genre
		self.coverArt = coverArt
		self.starredAt = starredAt
		self.refs = refs ?? [ref]
	}

	var id: ItemRef { ref }
	var isStarred: Bool { starredAt != nil }
	var sources: [ServerId] { refs.map(\.server) }
}

/// A track. **Songs never merge** — a track list always comes from one album on
/// one server — so there is no `refs` here, and its absence is the rule.
struct Song: Identifiable, Hashable, Sendable {
	let ref: ItemRef
	let title: String
	let artistName: String
	let albumTitle: String
	let albumRef: ItemRef?
	let track: Int?
	let discNumber: Int?
	let year: Int?
	let duration: Int
	let bitRate: Int?
	let suffix: String?
	let contentType: String?
	let sizeBytes: Int
	let coverArt: ItemRef?
	let starredAt: String?
	let lastPlayedAt: String?
	let isVideo: Bool
	/// False whenever the endpoint did not select the codec columns, which is
	/// the safe direction — the video still plays, it just seeks by
	/// re-request. Trusted as given rather than second-guessed.
	let nativeSeek: Bool
	let width: Int?
	let height: Int?

	var id: ItemRef { ref }
	var isStarred: Bool { starredAt != nil }
}

struct Playlist: Identifiable, Hashable, Sendable {
	let ref: ItemRef
	let name: String
	let comment: String?
	let owner: String?
	let isPublic: Bool
	let songCount: Int
	let duration: Int
	let songs: [Song]

	var id: ItemRef { ref }
}

struct ArtistInfo: Hashable, Sendable {
	let biography: String?
	let wikiUrl: String?
	let allMusicUrl: String?
	let lastFmUrl: String?
	let discogsUrl: String?
	let imageUrl: String?

	/// Worth asking before rendering a header that would otherwise be an empty
	/// box: the server answers `ok` with nothing in it for an artist
	/// MusicBrainz has never heard of.
	var isEmpty: Bool {
		(biography?.isEmpty ?? true) && wikiUrl == nil && allMusicUrl == nil
			&& lastFmUrl == nil && discogsUrl == nil
	}
}

struct AlbumNotes: Hashable, Sendable {
	let notes: String?
	let wikiUrl: String?
	let allMusicUrl: String?

	var isEmpty: Bool {
		(notes?.isEmpty ?? true) && wikiUrl == nil && allMusicUrl == nil
	}
}

struct AlbumDetail: Hashable, Sendable {
	let album: Album
	let songs: [Song]

	/// Disc headings appear only when there is more than one disc, so a
	/// single-disc album is not decorated with a heading that says nothing.
	var isMultiDisc: Bool {
		Set(songs.compactMap(\.discNumber)).count > 1
	}
}

/// The shape both `search3` and `getStarred2` answer with.
struct LibrarySelection: Hashable, Sendable {
	var artists: [Artist] = []
	var albums: [Album] = []
	var songs: [Song] = []

	var isEmpty: Bool { artists.isEmpty && albums.isEmpty && songs.isEmpty }
}

/// Which parameter name `star`/`unstar` should carry the id under.
enum StarKind: Sendable, Hashable {
	case song, album, artist
}
