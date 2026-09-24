//	GainDrive for iOS
//	Copyright (C) 2026 Kasper Peeters
//
//	This file is part of GainDrive, distributed under the GNU General
//	Public License version 3 or later, with the Application Distribution
//	Exception in ios/LICENSE. See LICENSE at the repository root for the
//	full text of the GPL.

import Foundation

/// The endpoint surface, mirroring `net/SubsonicApi.kt`.
///
/// These take bare `String` ids, and that is correct rather than a lapse: a
/// `SubsonicClient` **is** one server - it carries the `ServerId` - so an id
/// here is already qualified by the receiver. The composite-id rule binds every
/// layer above this one, which is where `LibraryRepository` enforces it.
///
/// Each method unwraps to the payload rather than returning an envelope, so a
/// caller never sees `SubsonicEnvelope`. A missing payload on an `ok` status
/// comes back as `nil` rather than throwing: the server said the request
/// succeeded, and inventing a failure from that would be the client
/// contradicting it.
extension SubsonicClient {

	// MARK: - Library roots

	func musicFolders() async throws -> [MusicFolderDto] {
		try await perform("getMusicFolders", expecting: GetMusicFoldersBody.self)
			.musicFolders?.musicFolder ?? []
	}

	// MARK: - Browsing

	/// `contentType` is a gaindrive extension restricting the listing to roots
	/// of one kind. **Only send it to a server that advertises kinds** - one
	/// that does not simply ignores it and answers with its whole library, and
	/// filtering the reply is too late because nothing in it says which rows to
	/// discard. `MusicFolderTypes` is what decides.
	/// Plain strings rather than the `RootRequest` that composes them, because
	/// that type lives in `Data` and `Net` may not reach up into it. The
	/// repository unpacks it, which is also the only place that knows the
	/// three are alternatives rather than a combination.
	///
	/// `personal` is `"true"` for this account's uploads and `"*"` for every
	/// account's, which the server allows only an admin. It switches to a
	/// different library altogether, so the other two are ignored while it is
	/// set.
	func artists(
		personal: String? = nil, contentType: String? = nil, musicFolderId: String? = nil
	) async throws -> [IndexDto] {
		var parameters: [String: String] = [:]
		if let personal { parameters["personal"] = personal }
		if let contentType { parameters["contentType"] = contentType }
		if let musicFolderId { parameters["musicFolderId"] = musicFolderId }
		return try await perform("getArtists", parameters: parameters, expecting: GetArtistsBody.self)
			.artists?.index ?? []
	}

	func artist(id: String) async throws -> ArtistWithAlbums? {
		try await perform("getArtist", parameters: ["id": id], expecting: GetArtistBody.self).artist
	}

	func album(id: String) async throws -> AlbumDto? {
		try await perform("getAlbum", parameters: ["id": id], expecting: GetAlbumBody.self).album
	}

	/// Answering this may send the *server* out to MusicBrainz and Wikipedia,
	/// so it can take seconds. Never await it before the thing the user asked
	/// for.
	func artistInfo2(id: String) async throws -> ArtistInfoDto? {
		try await perform("getArtistInfo2", parameters: ["id": id], expecting: GetArtistInfoBody.self)
			.artistInfo2
	}

	/// As `artistInfo2` - same upstream, same latency.
	func albumInfo2(id: String) async throws -> AlbumInfoDto? {
		try await perform("getAlbumInfo2", parameters: ["id": id], expecting: GetAlbumInfoBody.self)
			.albumInfo2
	}

	/// A gaindrive extension: how many images the album folder holds, cover
	/// included. Drives the `index` parameter of `getCoverArt`.
	func albumImageCount(id: String) async throws -> Int {
		try await perform("getAlbumImages", parameters: ["id": id], expecting: GetAlbumImagesBody.self)
			.albumImages?.count ?? 0
	}

	// MARK: - Search and stars

	/// A count of zero asks the server to skip that category entirely, which is
	/// what the filter chips switch off.
	///
	/// `chapterCount` is a gaindrive extension, and **is sent only when it is
	/// non-zero**: the server answers without a `chapter` array at all for a
	/// client that never asks, which is the reply every other Subsonic client
	/// sees, so not sending it is the one way to be sure a server that predates
	/// the extension is asked nothing new.
	func search3(
		query: String, artistCount: Int, albumCount: Int, songCount: Int,
		chapterCount: Int = 0
	) async throws -> SearchResultDto {
		var parameters = [
			"query": query,
			"artistCount": String(artistCount),
			"albumCount": String(albumCount),
			"songCount": String(songCount),
		]
		if chapterCount > 0 { parameters["chapterCount"] = String(chapterCount) }
		return try await perform("search3", parameters: parameters, expecting: Search3Body.self)
			.searchResult3 ?? SearchResultDto()
	}

	func starred2() async throws -> SearchResultDto {
		try await perform("getStarred2", expecting: Starred2Body.self).starred2 ?? SearchResultDto()
	}

	/// The parameter *name* selects the kind, and all three may be mixed in one
	/// call. Note the server stores stars **by path** and silently ignores an id
	/// it cannot resolve rather than answering error 70 - so this succeeding is
	/// not evidence that anything changed. See `StarStore`.
	func star(songIds: [String] = [], albumIds: [String] = [], artistIds: [String] = []) async throws {
		try await setStarred("star", songIds: songIds, albumIds: albumIds, artistIds: artistIds)
	}

	func unstar(songIds: [String] = [], albumIds: [String] = [], artistIds: [String] = []) async throws {
		try await setStarred("unstar", songIds: songIds, albumIds: albumIds, artistIds: artistIds)
	}

	private func setStarred(
		_ endpoint: String, songIds: [String], albumIds: [String], artistIds: [String]
	) async throws {
		let items =
			songIds.map { URLQueryItem(name: "id", value: $0) }
			+ albumIds.map { URLQueryItem(name: "albumId", value: $0) }
			+ artistIds.map { URLQueryItem(name: "artistId", value: $0) }
		guard !items.isEmpty else { return }
		_ = try await perform(endpoint, items: items, expecting: EmptyBody.self)
	}

	// MARK: - Playlists

	func playlists() async throws -> [PlaylistDto] {
		try await perform("getPlaylists", expecting: GetPlaylistsBody.self).playlists?.playlist ?? []
	}

	func playlist(id: String) async throws -> PlaylistDto? {
		try await perform("getPlaylist", parameters: ["id": id], expecting: GetPlaylistBody.self).playlist
	}

	/// Answers the created playlist in full, so the caller does not have to
	/// re-read it to learn its id.
	func createPlaylist(name: String, songIds: [String]) async throws -> PlaylistDto? {
		let items =
			[URLQueryItem(name: "name", value: name)]
			+ songIds.map { URLQueryItem(name: "songId", value: $0) }
		return try await perform("createPlaylist", items: items, expecting: GetPlaylistBody.self).playlist
	}

	/// **`playlistId`, not `id`** - the one endpoint that spells it
	/// differently, and it answers error 10 rather than doing something
	/// surprising if you get it wrong.
	///
	/// Removal is by *position*, and adds and removes are applied in the same
	/// call. Positions shift the moment one is removed, so the caller must not
	/// let a second request carry indices computed against the pre-removal
	/// list.
	func updatePlaylist(
		playlistId: String, songIdToAdd: [String] = [], songIndexToRemove: [Int] = []
	) async throws {
		let items =
			[URLQueryItem(name: "playlistId", value: playlistId)]
			+ songIdToAdd.map { URLQueryItem(name: "songIdToAdd", value: $0) }
			+ songIndexToRemove.map { URLQueryItem(name: "songIndexToRemove", value: String($0)) }
		_ = try await perform("updatePlaylist", items: items, expecting: EmptyBody.self)
	}

	func deletePlaylist(id: String) async throws {
		_ = try await perform("deletePlaylist", parameters: ["id": id], expecting: EmptyBody.self)
	}

	// MARK: - Video

	/// The subtitle tracks inside one video.
	///
	/// A film with none answers an empty list, and that is the whole of "this
	/// has no captions" - there is no flag to check and no track invented for
	/// it, unlike ExoPlayer's HLS extractor, which conjures a CEA-608 entry for
	/// a playlist declaring none.
	func videoInfo(id: String) async throws -> VideoInfoDto? {
		try await perform("getVideoInfo", parameters: ["id": id], expecting: GetVideoInfoBody.self)
			.videoInfo
	}

	/// One caption track, as WebVTT.
	///
	/// Everything is converted through ffmpeg's webvtt muxer server-side -
	/// including a file that was already `.vtt`, so a mislabelled one cannot be
	/// served verbatim. That is what leaves the client one format to parse.
	func captions(id: String, captionId: String) async throws -> String {
		try await text("getCaptions", parameters: ["id": id, "captionId": captionId])
	}

	// MARK: - Chapters

	/// The markers inside one recording.
	///
	/// **Reads the sidecar file on every call**, server-side, because a
	/// playback lookup has to be right - the indexed copy is what
	/// `albumChapters` reads. That split is deliberate and the two must not be
	/// collapsed here either: this is what a hand-edited sidecar shows up in
	/// immediately.
	/// The whole payload rather than its array, because `source` is part of the
	/// answer: markers read out of a video's container are **not** in the scan's
	/// index, so they appear in the player and not in the album listing, and
	/// that is worth saying out loud rather than looking like a bug.
	func chapters(id: String) async throws -> ChaptersBody.Payload? {
		try await perform("getChapters", parameters: ["id": id], expecting: ChaptersBody.self)
			.chapters
	}

	/// Every chaptered recording in one album folder, from the indexed table -
	/// one query rather than a file read per song, which is what makes drawing
	/// markers as an album's rows affordable.
	func albumChapters(id: String) async throws -> [AlbumChaptersBody.Recording] {
		try await perform(
			"getAlbumChapters", parameters: ["id": id], expecting: AlbumChaptersBody.self
		).albumChapters?.song ?? []
	}

	// MARK: - Playback reporting

	/// Tells the server a track is being, or has been, played.
	///
	/// `submission=false` is a now-playing notification. `submission=true`
	/// records a **completed** play, and is what increments the play count and
	/// sets `last_played` - which is therefore what makes `getRecentSongs`
	/// non-empty, in this app and in the other two clients, which read the same
	/// server-side state.
	func scrobble(id: String, submission: Bool) async throws {
		_ = try await perform(
			"scrobble",
			parameters: ["id": id, "submission": submission ? "true" : "false"],
			expecting: EmptyBody.self)
	}

	// MARK: - Recents

	/// A gaindrive extension. A server without it answers an error, which the
	/// repository degrades to an empty section rather than a failure - the
	/// feature being absent is not the server being down.
	func recentSongs(size: Int, offset: Int = 0) async throws -> [SongDto] {
		try await perform(
			"getRecentSongs",
			parameters: ["size": String(size), "offset": String(offset)],
			expecting: GetRecentSongsBody.self
		).recentSongs?.song ?? []
	}

	// MARK: - Casting

	/// A credential a Cast receiver can fetch one track with, so the URL we hand
	/// it need not carry ours.
	///
	/// This app holds the Cast control channel itself and builds the receiver's
	/// URLs, and a receiver has no account - so those URLs used to carry
	/// `u`/`t`/`s`, which together are the password: `t` is md5(password + salt)
	/// and `s` is the salt, and a television holding them reads the whole library
	/// as this person until the password changes.
	///
	/// One token covers the track's stream, its cover art and its subtitles,
	/// expires in twelve hours, and reaches nothing the account could not already
	/// read. A gaindrive extension, and a server without it answers an error -
	/// which `CastUrls.castToken(for:)` degrades to "carry on with the ordinary
	/// credentials" rather than surfacing, since that is exactly what this app
	/// did before the endpoint existed.
	func castToken(id: String) async throws -> String? {
		try await perform("getCastToken", parameters: ["id": id], expecting: CastTokenBody.self)
			.castToken
	}
}
