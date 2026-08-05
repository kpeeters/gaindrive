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

/// The fallbacks. DTOs are built by decoding JSON rather than by hand, so these
/// exercise the same path a real response takes.
struct LibraryMapperTests {
	private let server = ServerId()

	private func dto<T: Decodable>(_ json: String, as type: T.Type) throws -> T {
		try JSONDecoder().decode(type, from: Data(json.utf8))
	}

	/// `getAlbum` sends `name`, the directory-shaped endpoints send `title`.
	/// Either can be the one that is present.
	@Test func titleFallsBackBetweenNameAndTitle() throws {
		let named = try dto(#"{"id":"1","name":"The Wall"}"#, as: AlbumDto.self)
		#expect(LibraryMapper.album(named, server: server)?.title == "The Wall")

		let titled = try dto(#"{"id":"1","title":"The Wall"}"#, as: AlbumDto.self)
		#expect(LibraryMapper.album(titled, server: server)?.title == "The Wall")

		let neither = try dto(#"{"id":"1"}"#, as: AlbumDto.self)
		#expect(LibraryMapper.album(neither, server: server)?.title == "")
	}

	/// `artistId` is the ID3 field and `parent` the folder-browsing
	/// equivalent — prefer the former, accept the latter, because search and
	/// starred results carry only `parent`.
	@Test func artistRefPrefersArtistIdAndAcceptsParent() throws {
		let both = try dto(#"{"id":"1","artistId":"7","parent":"9"}"#, as: AlbumDto.self)
		#expect(LibraryMapper.album(both, server: server)?.artistRef?.id == "7")

		let parentOnly = try dto(#"{"id":"1","parent":"9"}"#, as: AlbumDto.self)
		#expect(LibraryMapper.album(parentOnly, server: server)?.artistRef?.id == "9")
	}

	/// The server uses the artist's own folder id as its cover art id, so an
	/// absent `coverArt` still has a portrait to ask for.
	@Test func artistCoverFallsBackToItsOwnId() throws {
		let artist = try dto(#"{"id":"5","name":"Pink Floyd"}"#, as: ArtistDto.self)
		#expect(LibraryMapper.artist(artist, server: server)?.coverArt?.id == "5")
	}

	@Test func songCoverFallsBackToTheAlbum() throws {
		let song = try dto(#"{"id":"3","title":"One","albumId":"2"}"#, as: SongDto.self)
		#expect(LibraryMapper.song(song, server: server)?.coverArt?.id == "2")
	}

	/// Directory-shaped results carry no count, so it falls back to what
	/// arrived rather than showing a confident zero.
	@Test func songCountFallsBackToTheTrackList() throws {
		let album = try dto(
			#"{"id":"1","song":[{"id":"3"},{"id":"4"}]}"#, as: AlbumDto.self)
		#expect(LibraryMapper.album(album, server: server)?.songCount == 2)
	}

	/// Subsonic uses 0 for "unknown" in every numeric field that can be one,
	/// and rendering it would put a 0 in front of every track on an album that
	/// simply has no numbering.
	@Test func zeroMeansUnknown() throws {
		let song = try dto(
			#"{"id":"3","track":0,"discNumber":0,"year":0,"bitRate":0}"#, as: SongDto.self)
		let mapped = try #require(LibraryMapper.song(song, server: server))
		#expect(mapped.track == nil)
		#expect(mapped.discNumber == nil)
		#expect(mapped.year == nil)
		#expect(mapped.bitRate == nil)
	}

	@Test func blankStringsBecomeNil() throws {
		let song = try dto(#"{"id":"3","genre":"  ","suffix":""}"#, as: SongDto.self)
		let mapped = try #require(LibraryMapper.song(song, server: server))
		#expect(mapped.suffix == nil)
	}

	/// An entry with no id cannot be addressed, so it is dropped rather than
	/// mapped into something that would build a URL the server answers 500 to.
	@Test func anEntryWithoutAnIdIsDropped() throws {
		let song = try dto(#"{"title":"Orphan"}"#, as: SongDto.self)
		#expect(LibraryMapper.song(song, server: server) == nil)

		let blank = try dto(#"{"id":"","title":"Orphan"}"#, as: SongDto.self)
		#expect(LibraryMapper.song(blank, server: server) == nil)
	}

	/// The safe direction: an endpoint that did not select the codec columns
	/// reports no `nativeSeek`, and false means "seek by re-request".
	@Test func absentNativeSeekIsFalse() throws {
		let song = try dto(#"{"id":"3","isVideo":true}"#, as: SongDto.self)
		let mapped = try #require(LibraryMapper.song(song, server: server))
		#expect(mapped.isVideo)
		#expect(mapped.nativeSeek == false)
	}

	/// Invisible with one server configured, which is exactly why it is tested:
	/// the same Subsonic id on two servers is not the same thing.
	@Test func theSameIdOnTwoServersMapsToTwoRefs() throws {
		let other = ServerId()
		let dto = try dto(#"{"id":"42","name":"Pink Floyd"}"#, as: ArtistDto.self)
		let one = LibraryMapper.artist(dto, server: server)
		let two = LibraryMapper.artist(dto, server: other)
		#expect(one?.ref != two?.ref)
	}

	@Test func playlistTracksComeFromEntry() throws {
		let playlist = try dto(
			#"{"id":"7","name":"Mix","entry":[{"id":"3","title":"One"}]}"#, as: PlaylistDto.self)
		#expect(LibraryMapper.playlist(playlist, server: server)?.songs.count == 1)
	}

	/// The server answers `ok` with an empty object for an artist MusicBrainz
	/// has never heard of, and a header built from that would be an empty box.
	@Test func emptyProseIsRecognisable() throws {
		let info = try dto("{}", as: ArtistInfoDto.self)
		#expect(LibraryMapper.artistInfo(info).isEmpty)

		let notes = try dto(#"{"notes":"Recorded in 1979."}"#, as: AlbumInfoDto.self)
		#expect(!LibraryMapper.albumNotes(notes).isEmpty)
	}
}
