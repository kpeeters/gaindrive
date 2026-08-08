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

/// The endpoints whose parameter names and ordering are traps, asserted on the
/// outgoing URL — the same style as `AuthParametersTests`, and for the same
/// reason: `SubsonicClient.url` is a pure function, so its output *is* the
/// request.
struct PlaylistEditTests {
	private let client = SubsonicClient(
		serverId: ServerId(),
		baseURL: URL(string: "http://192.0.2.9:4040")!,
		auth: AuthParameters(username: "admin", password: "secret", salt: "aa11"))

	private func values(_ url: URL, _ name: String) -> [String] {
		let items = URLComponents(url: url, resolvingAgainstBaseURL: false)?.queryItems ?? []
		return items.filter { $0.name == name }.compactMap(\.value)
	}

	/// **`playlistId`, not `id`.** The one endpoint that spells it differently,
	/// and it answers error 10 rather than doing something surprising when you
	/// get it wrong.
	@Test func updatePlaylistNamesThePlaylistIdParameter() {
		let url = client.url(
			"updatePlaylist", items: [URLQueryItem(name: "playlistId", value: "7")])
		#expect(values(url, "playlistId") == ["7"])
		#expect(values(url, "id").isEmpty)
	}

	/// A dictionary cannot express this at all, which is why the query-item
	/// overload exists.
	@Test func repeatedParametersAllSurvive() {
		let url = client.url(
			"updatePlaylist",
			items: [URLQueryItem(name: "playlistId", value: "7")]
				+ ["3", "4", "5"].map { URLQueryItem(name: "songIdToAdd", value: $0) })
		#expect(values(url, "songIdToAdd") == ["3", "4", "5"])
	}

	/// **The ordering test.** `songIndexToRemove` is positional, so a sort that
	/// reordered equal names would remove the wrong tracks. The URL sort is by
	/// name only, and stable.
	@Test func repeatedValuesKeepTheCallersOrder() {
		let url = client.url(
			"updatePlaylist",
			items: [URLQueryItem(name: "playlistId", value: "7")]
				+ [9, 2, 5].map { URLQueryItem(name: "songIndexToRemove", value: String($0)) })
		#expect(values(url, "songIndexToRemove") == ["9", "2", "5"])
	}

	/// Interleaved names still sort together without disturbing each group's
	/// internal order.
	@Test func interleavedNamesGroupWithoutReordering() {
		let url = client.url(
			"updatePlaylist",
			items: [
				URLQueryItem(name: "songIdToAdd", value: "a"),
				URLQueryItem(name: "songIndexToRemove", value: "1"),
				URLQueryItem(name: "songIdToAdd", value: "b"),
				URLQueryItem(name: "songIndexToRemove", value: "0"),
			])
		#expect(values(url, "songIdToAdd") == ["a", "b"])
		#expect(values(url, "songIndexToRemove") == ["1", "0"])
	}

	/// The parameter *name* selects the kind, and the wrong one silently stars
	/// nothing — the server ignores an id it cannot resolve rather than
	/// answering an error.
	@Test func starNamesTheParameterByKind() {
		let song = client.url("star", items: [URLQueryItem(name: "id", value: "3")])
		#expect(values(song, "id") == ["3"])
		#expect(values(song, "albumId").isEmpty)

		let album = client.url("star", items: [URLQueryItem(name: "albumId", value: "2")])
		#expect(values(album, "albumId") == ["2"])
		#expect(values(album, "id").isEmpty)

		let artist = client.url("unstar", items: [URLQueryItem(name: "artistId", value: "1")])
		#expect(values(artist, "artistId") == ["1"])
	}

	/// The auth parameters still arrive, and the password still does not — the
	/// query-item overload must not have opened a hole in what phase 1 proved.
	@Test func theItemOverloadStillCarriesAuth() {
		let url = client.url("updatePlaylist", items: [URLQueryItem(name: "playlistId", value: "7")])
		#expect(values(url, "u") == ["admin"])
		#expect(values(url, "t").count == 1)
		#expect(values(url, "p").isEmpty)
		#expect(!url.absoluteString.contains("secret"))
	}

	/// Search sends arbitrary user text, so it is the first feature that can
	/// put a `&` or a `+` into a value.
	@Test func aQueryWithSeparatorsSurvivesIntact() {
		let url = client.url("search3", parameters: ["query": "rock & roll+jazz"])
		#expect(values(url, "query") == ["rock & roll+jazz"])
		#expect(url.absoluteString.contains("%26"))
		#expect(url.absoluteString.contains("%2B"))
	}
}
