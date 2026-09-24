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

/// Real payload shapes, per endpoint.
///
/// The shapes here are the ones gaindrive actually emits, including the several
/// places it is not uniform - which is precisely where a client goes wrong
/// silently and only against some endpoints.
struct BrowseDtoTests {
	private func decode<Body: Decodable & Sendable>(
		_ json: String, expecting type: Body.Type
	) throws -> Body {
		try SubsonicClient.decode(Data(json.utf8), expecting: type, httpStatus: 200)
	}

	@Test func getArtistsNestsIndexesUnderArtists() throws {
		let body: GetArtistsBody = try decode(
			"""
			{"subsonic-response":{"status":"ok","artists":{"ignoredArticles":"The",
			 "index":[{"name":"P","artist":[{"id":"1","name":"Pink Floyd","albumCount":15}]}]}}}
			""", expecting: GetArtistsBody.self)
		#expect(body.artists?.index.first?.name == "P")
		#expect(body.artists?.index.first?.artist.first?.albumCount == 15)
	}

	/// A server with exactly one artist in one bucket collapses both levels.
	@Test func getArtistsSurvivesBothLevelsCollapsing() throws {
		let body: GetArtistsBody = try decode(
			"""
			{"subsonic-response":{"status":"ok","artists":
			 {"index":{"name":"P","artist":{"id":"1","name":"Pink Floyd"}}}}}
			""", expecting: GetArtistsBody.self)
		#expect(body.artists?.index.count == 1)
		#expect(body.artists?.index.first?.artist.count == 1)
	}

	/// `getArtist`'s album rows carry **both** `title` and `name`, while the
	/// artist object itself carries no `coverArt`.
	@Test func getArtistAlbumsCarryBothTitleAndName() throws {
		let body: GetArtistBody = try decode(
			"""
			{"subsonic-response":{"status":"ok","artist":{"id":"1","name":"Pink Floyd",
			 "albumCount":1,"album":[{"id":"2","parent":"1","artistId":"1",
			 "title":"The Wall","name":"The Wall","artist":"Pink Floyd","songCount":26}]}}}
			""", expecting: GetArtistBody.self)
		let album = try #require(body.artist?.album.first)
		#expect(album.title == "The Wall")
		#expect(album.name == "The Wall")
		#expect(body.artist?.coverArt == nil)
	}

	/// `getAlbum` sends `name` and **no `title`** - the opposite of the
	/// directory-shaped endpoints.
	@Test func getAlbumSendsNameWithoutTitle() throws {
		let body: GetAlbumBody = try decode(
			"""
			{"subsonic-response":{"status":"ok","album":{"id":"2","parent":"1","artistId":"1",
			 "name":"The Wall","artist":"Pink Floyd","songCount":1,"duration":200,
			 "song":[{"id":"3","title":"In the Flesh?","duration":199,"track":1}]}}}
			""", expecting: GetAlbumBody.self)
		#expect(body.album?.name == "The Wall")
		#expect(body.album?.title == nil)
		#expect(body.album?.song.count == 1)
	}

	/// Search albums are directory-shaped: no `songCount`, `duration`, `year`
	/// or `starred`, and `album` duplicates `title`.
	@Test func search3AlbumsAreDirectoryShaped() throws {
		let body: Search3Body = try decode(
			"""
			{"subsonic-response":{"status":"ok","searchResult3":{
			 "artist":[{"id":"1","name":"Pink Floyd"}],
			 "album":[{"id":"2","parent":"1","isDir":true,"title":"The Wall",
			           "artist":"Pink Floyd","album":"The Wall"}],
			 "song":[{"id":"3","title":"Hey You"}]}}}
			""", expecting: Search3Body.self)
		let album = try #require(body.searchResult3?.album.first)
		#expect(album.songCount == nil)
		#expect(album.artistId == nil)
		#expect(album.parent == "2" || album.parent == "1")
		#expect(body.searchResult3?.artist.first?.albumCount == nil)
	}

	/// **`entry`, not `song`.** The one collection spelled differently, and the
	/// test that would catch the obvious mistake.
	@Test func getPlaylistKeysItsTracksAsEntry() throws {
		let body: GetPlaylistBody = try decode(
			"""
			{"subsonic-response":{"status":"ok","playlist":{"id":"7","name":"Mix",
			 "public":false,"songCount":2,"duration":400,
			 "entry":[{"id":"3","title":"One"},{"id":"4","title":"Two"}]}}}
			""", expecting: GetPlaylistBody.self)
		#expect(body.playlist?.entry.count == 2)
		#expect(body.playlist?.isPublic == false)
	}

	/// `lastPlayed` is raw SQLite - no `T`, no `Z` - unlike every other
	/// timestamp in the API. It has to survive as a string for `relativeTime`
	/// to deal with.
	@Test func recentSongsKeepTheSQLiteTimestamp() throws {
		let body: GetRecentSongsBody = try decode(
			"""
			{"subsonic-response":{"status":"ok","recentSongs":{"song":[
			 {"id":"3","title":"One","lastPlayed":"2026-08-05 11:22:33"}]}}}
			""", expecting: GetRecentSongsBody.self)
		#expect(body.recentSongs?.song.first?.lastPlayed == "2026-08-05 11:22:33")
	}

	/// `starred` is a timestamp present only when starred. There is no boolean
	/// form and no present-and-false, so its presence *is* the star.
	@Test func starredIsATimestampOrAbsent() throws {
		let starred: GetAlbumBody = try decode(
			#"{"subsonic-response":{"status":"ok","album":{"id":"2","starred":"2026-01-02T03:04:05Z"}}}"#,
			expecting: GetAlbumBody.self)
		#expect(starred.album?.starred != nil)

		let plain: GetAlbumBody = try decode(
			#"{"subsonic-response":{"status":"ok","album":{"id":"2"}}}"#,
			expecting: GetAlbumBody.self)
		#expect(plain.album?.starred == nil)
	}

	@Test func musicFolderWithoutContentTypeDecodes() throws {
		let body: GetMusicFoldersBody = try decode(
			"""
			{"subsonic-response":{"status":"ok","musicFolders":{"musicFolder":[
			 {"id":"1","name":"Music","contentType":"artists"},{"id":"2","name":"Other"}]}}}
			""", expecting: GetMusicFoldersBody.self)
		#expect(body.musicFolders?.musicFolder.count == 2)
		#expect(body.musicFolders?.musicFolder.last?.contentType == nil)
	}

	@Test func albumImagesCarriesACount() throws {
		let body: GetAlbumImagesBody = try decode(
			#"{"subsonic-response":{"status":"ok","albumImages":{"count":4}}}"#,
			expecting: GetAlbumImagesBody.self)
		#expect(body.albumImages?.count == 4)
	}

	/// An OpenSubsonic server adding fields - or a whole nested object - must
	/// be a non-event.
	@Test func unknownFieldsAreIgnored() throws {
		let body: GetAlbumBody = try decode(
			"""
			{"subsonic-response":{"status":"ok","openSubsonic":true,"album":
			 {"id":"2","name":"X","futureField":9,"nested":{"a":[1,2]},
			  "song":[{"id":"3","title":"One","alsoNew":"?"}]}}}
			""", expecting: GetAlbumBody.self)
		#expect(body.album?.song.first?.title == "One")
	}

	/// Every payload carrying only its mandatory field has to map without
	/// throwing - the tolerance property, asserted at the DTO level.
	@Test func minimalPayloadsDecode() throws {
		_ = try decode(
			#"{"subsonic-response":{"status":"ok","album":{"id":"2"}}}"#,
			expecting: GetAlbumBody.self)
		_ = try decode(
			#"{"subsonic-response":{"status":"ok","artist":{"id":"1"}}}"#,
			expecting: GetArtistBody.self)
		_ = try decode(
			#"{"subsonic-response":{"status":"ok","playlist":{"id":"7"}}}"#,
			expecting: GetPlaylistBody.self)
		_ = try decode(
			#"{"subsonic-response":{"status":"ok","searchResult3":{}}}"#,
			expecting: Search3Body.self)
	}

	/// A body whose container is absent entirely - which is what a server
	/// answers for a search that matched nothing.
	@Test func absentContainerIsNil() throws {
		let body: Search3Body = try decode(
			#"{"subsonic-response":{"status":"ok"}}"#, expecting: Search3Body.self)
		#expect(body.searchResult3 == nil)
	}
}
