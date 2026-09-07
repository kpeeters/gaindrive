//	GainDrive for iOS
//	Copyright (C) 2026 Kasper Peeters
//
//	This file is part of GainDrive, distributed under the GNU General
//	Public License version 3 or later, with the Application Distribution
//	Exception in ios/LICENSE. See LICENSE at the repository root for the
//	full text of the GPL.

import Foundation

/// DTO → domain, mirroring `data/LibraryMapper.kt`.
///
/// **This is the only place an absent optional becomes something renderable.**
/// The DTOs deliberately have no fallbacks and the screens deliberately have no
/// optionals, so every "what does missing mean here" decision is made once, in
/// this file, where the answers can be compared against each other.
enum LibraryMapper {

	// MARK: - Artists

	static func artist(_ dto: ArtistDto, server: ServerId) -> Artist? {
		guard let ref = server.ref(dto.id) else { return nil }
		return Artist(
			ref: ref,
			name: dto.name ?? "",
			albumCount: dto.albumCount ?? 0,
			// The server uses the artist's own folder id as its cover art id,
			// so an absent `coverArt` still has a portrait to ask for.
			coverArt: server.ref(dto.coverArt) ?? ref,
			starredAt: dto.starred
		)
	}

	static func artist(_ dto: ArtistWithAlbums, server: ServerId) -> Artist? {
		guard let ref = server.ref(dto.id) else { return nil }
		return Artist(
			ref: ref,
			name: dto.name ?? "",
			// `getArtist` answers an albumCount, but the album list it also
			// sends is the more reliable of the two.
			albumCount: dto.albumCount ?? dto.album.count,
			coverArt: server.ref(dto.coverArt) ?? ref,
			starredAt: dto.starred
		)
	}

	static func index(_ dto: IndexDto, server: ServerId) -> ArtistIndex {
		ArtistIndex(
			label: dto.name ?? "#",
			artists: dto.artist.compactMap { artist($0, server: server) })
	}

	// MARK: - Albums

	static func album(_ dto: AlbumDto, server: ServerId) -> Album? {
		guard let ref = server.ref(dto.id) else { return nil }
		return Album(
			ref: ref,
			// `getAlbum` sends `name`; the directory-shaped endpoints send
			// `title`. Either can be the one that is present.
			title: firstNonEmpty(dto.name, dto.title) ?? "",
			artistName: dto.artist ?? "",
			// `artistId` is the ID3 field and `parent` is the folder-browsing
			// equivalent. Prefer the former, accept the latter — search and
			// starred results carry only `parent`.
			artistRef: server.ref(dto.artistId) ?? server.ref(dto.parent),
			// Directory-shaped results carry no count, so fall back to what
			// arrived rather than showing a confident zero.
			songCount: positive(dto.songCount) ?? dto.song.count,
			videoCount: positive(dto.videoCount) ?? 0,
			duration: dto.duration ?? 0,
			year: positive(dto.year),
			genre: nonEmpty(dto.genre),
			coverArt: server.ref(dto.coverArt) ?? ref,
			starredAt: dto.starred
		)
	}

	static func musicRoot(_ dto: MusicFolderDto) -> MusicRoot? {
		guard let id = nonEmpty(dto.id) else { return nil }
		return MusicRoot(
			id: id, name: dto.name ?? id, contentType: nonEmpty(dto.contentType))
	}

	// MARK: - Songs

	static func song(_ dto: SongDto, server: ServerId) -> Song? {
		guard let ref = server.ref(dto.id) else { return nil }
		let albumRef = server.ref(dto.albumId) ?? server.ref(dto.parent)
		return Song(
			ref: ref,
			title: dto.title ?? "",
			artistName: dto.artist ?? "",
			albumTitle: dto.album ?? "",
			albumRef: albumRef,
			// A zero track or disc number means "not set" rather than "track
			// zero", and rendering it would put a 0 in front of every track on
			// an album that simply has no numbering.
			track: positive(dto.track),
			discNumber: positive(dto.discNumber),
			year: positive(dto.year),
			duration: dto.duration ?? 0,
			bitRate: positive(dto.bitRate),
			suffix: nonEmpty(dto.suffix),
			contentType: nonEmpty(dto.contentType),
			sizeBytes: dto.size ?? 0,
			// Cover art falls back to the album's, since a track in a folder
			// shares the folder's artwork.
			coverArt: server.ref(dto.coverArt) ?? albumRef,
			starredAt: dto.starred,
			lastPlayedAt: dto.lastPlayed,
			isVideo: dto.isVideo ?? false,
			nativeSeek: dto.nativeSeek ?? false,
			width: positive(dto.originalWidth),
			height: positive(dto.originalHeight)
		)
	}

	// MARK: - Playlists

	static func playlist(_ dto: PlaylistDto, server: ServerId) -> Playlist? {
		guard let ref = server.ref(dto.id) else { return nil }
		let songs = dto.entry.compactMap { song($0, server: server) }
		return Playlist(
			ref: ref,
			name: dto.name ?? "",
			comment: nonEmpty(dto.comment),
			owner: nonEmpty(dto.owner),
			isPublic: dto.isPublic ?? false,
			songCount: positive(dto.songCount) ?? songs.count,
			duration: dto.duration ?? 0,
			songs: songs
		)
	}

	// MARK: - Prose

	static func artistInfo(_ dto: ArtistInfoDto) -> ArtistInfo {
		ArtistInfo(
			biography: nonEmpty(dto.biography),
			wikiUrl: nonEmpty(dto.wikiUrl),
			allMusicUrl: nonEmpty(dto.allMusicUrl),
			lastFmUrl: nonEmpty(dto.lastFmUrl),
			discogsUrl: nonEmpty(dto.discogsUrl),
			imageUrl: nonEmpty(dto.largeImageUrl))
	}

	static func albumNotes(_ dto: AlbumInfoDto) -> AlbumNotes {
		AlbumNotes(
			notes: nonEmpty(dto.notes),
			wikiUrl: nonEmpty(dto.wikiUrl),
			allMusicUrl: nonEmpty(dto.allMusicUrl))
	}

	// MARK: - Selections

	static func selection(_ dto: SearchResultDto, server: ServerId) -> LibrarySelection {
		LibrarySelection(
			artists: dto.artist.compactMap { artist($0, server: server) },
			albums: dto.album.compactMap { album($0, server: server) },
			songs: dto.song.compactMap { song($0, server: server) })
	}

	// MARK: - Small decisions, made once

	private static func nonEmpty(_ value: String?) -> String? {
		guard let value, !value.trimmingCharacters(in: .whitespacesAndNewlines).isEmpty else {
			return nil
		}
		return value
	}

	private static func firstNonEmpty(_ values: String?...) -> String? {
		values.lazy.compactMap(nonEmpty).first
	}

	/// Subsonic uses 0 for "unknown" in every numeric field that can be one.
	private static func positive(_ value: Int?) -> Int? {
		guard let value, value > 0 else { return nil }
		return value
	}
}

/// Mirrors Android's `private fun ServerId.ref(id: String?)`. A method rather
/// than a free function so a local named `ref` — which every mapper below has
/// — cannot shadow it.
extension ServerId {
	/// Blank-safe: a field the server sent as an empty string means the same
	/// as one it omitted, and an `ItemRef` with an empty id builds a URL that
	/// answers 500.
	fileprivate func ref(_ id: String?) -> ItemRef? {
		guard let id, !id.isEmpty else { return nil }
		return ItemRef(server: self, id: id)
	}
}
