//	GainDrive for iOS
//	Copyright (C) 2026 Kasper Peeters
//
//	This file is part of GainDrive, distributed under the GNU General
//	Public License version 3 or later, with the Application Distribution
//	Exception in ios/LICENSE. See LICENSE at the repository root for the
//	full text of the GPL.

import Foundation

//	Shaped to the JSON the server actually emits rather than to the
//	specification's prose, and named after `net/BrowseDto.kt` so the two clients
//	can be read side by side.
//
//	One rule throughout, so conformance is checkable by inspection rather than
//	by judgement at every field:
//
//	  * every scalar is `@Loose` and therefore optional
//	  * every list is `@Listed`
//	  * every nested object is a plain `T?`
//
//	`decodeIfPresent` already handles an absent or null nested object, and one
//	arriving as a scalar *should* fail loudly. The consequence of the rule is
//	that a browse DTO has no throwing path at all except a container that is
//	not an object — which is precisely the tolerance property, and what the
//	tests assert.
//
//	Deciding what an absent field *means* is `LibraryMapper`'s job, not a
//	DTO's. Nothing here has a fallback.

// MARK: - Entities

struct ArtistDto: Decodable, Sendable {
	@Loose var id: String?
	@Loose var name: String?
	@Loose var albumCount: Int?
	@Loose var coverArt: String?
	/// A timestamp, present only when starred. Never a boolean and never
	/// present-and-false, so `starred != nil` *is* the star.
	@Loose var starred: String?
}

struct IndexDto: Decodable, Sendable {
	/// The bucket letter, or `#` for everything that does not start with one.
	@Loose var name: String?
	@Listed var artist: [ArtistDto] = []
}

/// `getArtist`'s payload. Separate from `ArtistDto` because it carries albums
/// and, unlike the rows in `getArtists`, **no `coverArt`**.
struct ArtistWithAlbums: Decodable, Sendable {
	@Loose var id: String?
	@Loose var name: String?
	@Loose var albumCount: Int?
	@Loose var coverArt: String?
	@Loose var starred: String?
	@Listed var album: [AlbumDto] = []
}

struct AlbumDto: Decodable, Sendable {
	@Loose var id: String?
	@Loose var parent: String?
	@Loose var artistId: String?
	/// `getAlbum` sends `name`; the directory-shaped endpoints send `title`.
	/// Either can be the one that is present, and `getArtist` sends both.
	@Loose var name: String?
	@Loose var title: String?
	@Loose var artist: String?
	@Loose var album: String?
	@Loose var songCount: Int?
	@Loose var duration: Int?
	@Loose var created: String?
	@Loose var coverArt: String?
	@Loose var year: Int?
	@Loose var genre: String?
	@Loose var starred: String?
	@Listed var song: [SongDto] = []
}

struct SongDto: Decodable, Sendable {
	@Loose var id: String?
	@Loose var parent: String?
	@Loose var albumId: String?
	@Loose var title: String?
	@Loose var artist: String?
	@Loose var album: String?
	@Loose var track: Int?
	@Loose var discNumber: Int?
	@Loose var year: Int?
	@Loose var genre: String?
	@Loose var size: Int?
	@Loose var contentType: String?
	@Loose var suffix: String?
	@Loose var duration: Int?
	@Loose var bitRate: Int?
	@Loose var coverArt: String?
	@Loose var starred: String?
	/// `getRecentSongs` only, and **raw SQLite `CURRENT_TIMESTAMP`** rather
	/// than the ISO 8601 every other timestamp uses. See `relativeTime`.
	@Loose var lastPlayed: String?

	@Loose var isVideo: Bool?
	/// Computed server-side from codec columns that only `getAlbum`,
	/// `getSong`, `getMusicDirectory` and `getVideos` select — so it is absent,
	/// and therefore taken as false, on recents, starred, search and
	/// playlists. That is the safe direction (the video plays and seeks by
	/// re-request), and the flag is trusted as given rather than
	/// second-guessed.
	@Loose var nativeSeek: Bool?
	@Loose var originalWidth: Int?
	@Loose var originalHeight: Int?
}

struct PlaylistDto: Decodable, Sendable {
	@Loose var id: String?
	@Loose var name: String?
	@Loose var comment: String?
	@Loose var owner: String?
	@Loose var isPublic: Bool?
	@Loose var songCount: Int?
	@Loose var duration: Int?
	@Loose var created: String?
	@Loose var changed: String?
	/// **`entry`, not `song`.** The one collection in the API that is spelled
	/// differently, and an easy thing to get wrong twice.
	@Listed var entry: [SongDto] = []

	// Spelled out because `public` is a keyword, which forces the whole set to
	// be written rather than synthesised.
	enum CodingKeys: String, CodingKey {
		case id, name, comment, owner, songCount, duration, created, changed, entry
		case isPublic = "public"
	}
}

struct MusicFolderDto: Decodable, Sendable {
	@Loose var id: String?
	@Loose var name: String?
	/// A gaindrive extension: `artists` or `categories`. Absent on a server
	/// that has no concept of root kinds — which is not the same as having
	/// none of that kind. See `MusicFolderTypes`.
	@Loose var contentType: String?
}

struct SearchResultDto: Decodable, Sendable {
	@Listed var artist: [ArtistDto] = []
	@Listed var album: [AlbumDto] = []
	@Listed var song: [SongDto] = []
}

struct ArtistInfoDto: Decodable, Sendable {
	@Loose var biography: String?
	@Loose var musicBrainzId: String?
	@Loose var lastFmUrl: String?
	@Loose var wikiUrl: String?
	@Loose var allMusicUrl: String?
	@Loose var discogsUrl: String?
	@Loose var largeImageUrl: String?
}

struct AlbumInfoDto: Decodable, Sendable {
	@Loose var notes: String?
	@Loose var musicBrainzId: String?
	@Loose var wikiUrl: String?
	@Loose var allMusicUrl: String?
}

// MARK: - Response bodies

//	One per endpoint, each holding the container the payload actually sits in.
//	The containers are separate types rather than nested dictionaries because
//	the nesting is part of the API and worth being able to see.

struct ArtistsContainer: Decodable, Sendable {
	@Loose var ignoredArticles: String?
	@Listed var index: [IndexDto] = []
}

struct GetArtistsBody: Decodable, Sendable {
	let artists: ArtistsContainer?
}

struct GetArtistBody: Decodable, Sendable {
	let artist: ArtistWithAlbums?
}

struct GetAlbumBody: Decodable, Sendable {
	let album: AlbumDto?
}

struct GetArtistInfoBody: Decodable, Sendable {
	let artistInfo2: ArtistInfoDto?
}

struct GetAlbumInfoBody: Decodable, Sendable {
	let albumInfo2: AlbumInfoDto?
}

struct MusicFoldersContainer: Decodable, Sendable {
	@Listed var musicFolder: [MusicFolderDto] = []
}

struct GetMusicFoldersBody: Decodable, Sendable {
	let musicFolders: MusicFoldersContainer?
}

struct Search3Body: Decodable, Sendable {
	let searchResult3: SearchResultDto?
}

struct Starred2Body: Decodable, Sendable {
	let starred2: SearchResultDto?
}

struct PlaylistsContainer: Decodable, Sendable {
	@Listed var playlist: [PlaylistDto] = []
}

struct GetPlaylistsBody: Decodable, Sendable {
	let playlists: PlaylistsContainer?
}

struct GetPlaylistBody: Decodable, Sendable {
	let playlist: PlaylistDto?
}

struct RecentSongsContainer: Decodable, Sendable {
	@Listed var song: [SongDto] = []
}

struct GetRecentSongsBody: Decodable, Sendable {
	let recentSongs: RecentSongsContainer?
}

/// A gaindrive extension, and JSON-only regardless of `f`.
struct AlbumImagesContainer: Decodable, Sendable {
	@Loose var count: Int?
}

struct GetAlbumImagesBody: Decodable, Sendable {
	let albumImages: AlbumImagesContainer?
}
