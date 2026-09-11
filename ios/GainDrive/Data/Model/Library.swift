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
//
//	They are `Codable` for the mirror, which stores mapped domain values rather
//	than raw responses — see `LibraryMirror`. That is also the whole of the
//	mirror's migration story: a value written by a build with a different shape
//	fails to decode, which reads as a miss, which is a re-fetch. A cache is
//	allowed to be thrown away.

/// One of a server's configured library roots.
///
/// **`id` is a bare `String`, and that is not a lapse in the composite-id
/// rule.** A root id is only meaningful to the server that issued it, and this
/// one never travels: it is read from that server's `getMusicFolders` and put
/// straight back into a request to the same server. Nothing above `Data` ever
/// sees it — the merged listing is asked for by root *kind*
/// (`LibraryRoots.listingRequests`), never by a root's own id.
struct MusicRoot: Hashable, Sendable, Codable {
	let id: String
	let name: String
	/// A gaindrive extension: `artists` or `categories`. Absent on a server
	/// with no concept of root kinds — which is **not** the same as having no
	/// roots of that kind.
	let contentType: String?
}

/// An artist, possibly standing for the same artist on several servers.
struct Artist: Identifiable, Hashable, Sendable, Codable {
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
struct ArtistIndex: Identifiable, Hashable, Sendable, Codable {
	let label: String
	let artists: [Artist]

	var id: String { label }
}

struct Album: Identifiable, Hashable, Sendable, Codable {
	let ref: ItemRef
	let title: String
	let artistName: String
	let artistRef: ItemRef?
	let songCount: Int
	/// How many of this album's tracks are video, which is the only thing that
	/// tells a season or a film from a record before its tracks are fetched —
	/// `isVideo` is a per-song field.
	///
	/// **Zero does not mean "no video".** The server does not carry it on the
	/// directory-shaped listings, so an album reached through search or starred
	/// reports zero whatever it holds. That is why the mark is only ever added
	/// and there is no "audio" counterpart: a wrong positive would be a lie,
	/// and a missing one is silence.
	let videoCount: Int
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
		songCount: Int = 0, videoCount: Int = 0, duration: Int = 0, year: Int? = nil,
		genre: String? = nil, coverArt: ItemRef? = nil, starredAt: String? = nil,
		refs: [ItemRef]? = nil
	) {
		self.ref = ref
		self.title = title
		self.artistName = artistName
		self.artistRef = artistRef
		self.songCount = songCount
		self.videoCount = videoCount
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
struct Song: Identifiable, Hashable, Sendable, Codable {
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

struct Playlist: Identifiable, Hashable, Sendable, Codable {
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

struct AlbumDetail: Hashable, Sendable, Codable {
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
	/// **A list of its own, never entries among the songs**, which is how the
	/// server sends it and for its reason: a chapter has no id anything can
	/// stream, star or queue, so a client told it was a song would be handed a
	/// track that does not work.
	var chapters: [ChapterHit] = []

	var isEmpty: Bool {
		artists.isEmpty && albums.isEmpty && songs.isEmpty && chapters.isEmpty
	}
}

/// Which parameter name `star`/`unstar` should carry the id under.
enum StarKind: Sendable, Hashable {
	case song, album, artist
}
