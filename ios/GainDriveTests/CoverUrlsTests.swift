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

/// The cover-art URL and, more importantly, the key it is filed under.
struct CoverUrlsTests {
	private let server = ServerId()

	private func urls(salt: String = "aa11") -> CoverUrls {
		CoverUrls(clients: [
			server: SubsonicClient(
				serverId: server,
				baseURL: URL(string: "http://192.0.2.9:4040")!,
				auth: AuthParameters(username: "admin", password: "secret", salt: salt))
		])
	}

	private func query(_ url: URL) -> [String: String] {
		let items = URLComponents(url: url, resolvingAgainstBaseURL: false)?.queryItems ?? []
		return Dictionary(items.compactMap { item in item.value.map { (item.name, $0) } }) {
			first, _ in first
		}
	}

	@Test func buildsTheCoverArtRequest() throws {
		let source = try #require(
			urls().source(ItemRef(server: server, id: "42"), size: 144))
		let parameters = query(source.url)
		#expect(source.url.path() == "/rest/getCoverArt.view")
		#expect(parameters["id"] == "42")
		#expect(parameters["size"] == "144")
		#expect(parameters["index"] == nil)
		#expect(parameters["p"] == nil)
		#expect(!source.url.absoluteString.contains("secret"))
	}

	@Test func indexSelectsAnExtraImage() throws {
		let source = try #require(
			urls().source(ItemRef(server: server, id: "42"), size: 800, index: 2))
		#expect(query(source.url)["index"] == "2")
	}

	/// **The test that proves the disk cache can hit across launches.**
	///
	/// The salt is regenerated every session, so the URL differs between two
	/// runs for the same bytes. The key must not, or the cache misses on every
	/// cold start - which is the entire reason `ImageStore` exists rather than
	/// a URL-keyed loader.
	@Test func theCacheKeyDoesNotContainTheSalt() throws {
		let first = try #require(urls(salt: "aaaa").source(ItemRef(server: server, id: "42"), size: 144))
		let second = try #require(urls(salt: "bbbb").source(ItemRef(server: server, id: "42"), size: 144))

		#expect(first.url != second.url)
		#expect(first.cacheKey == second.cacheKey)
		#expect(!first.cacheKey.contains("aaaa"))
	}

	/// Size is part of the key, so a thumbnail and a hero are two entries - and
	/// asking for a different size each time would be a cache that never hits.
	@Test func theCacheKeyVariesBySizeAndIndex() throws {
		let ref = ItemRef(server: server, id: "42")
		let thumb = try #require(urls().source(ref, size: 144))
		let hero = try #require(urls().source(ref, size: 800))
		let extra = try #require(urls().source(ref, size: 800, index: 1))

		#expect(thumb.cacheKey != hero.cacheKey)
		#expect(hero.cacheKey != extra.cacheKey)
	}

	/// The same Subsonic id on two servers is two different covers.
	@Test func theCacheKeyVariesByServer() throws {
		let other = ServerId()
		let otherUrls = CoverUrls(clients: [
			other: SubsonicClient(
				serverId: other,
				baseURL: URL(string: "http://other:4040")!,
				auth: AuthParameters(username: "admin", password: "secret", salt: "aa11"))
		])
		let mine = try #require(urls().source(ItemRef(server: server, id: "42"), size: 144))
		let theirs = try #require(otherUrls.source(ItemRef(server: other, id: "42"), size: 144))
		#expect(mine.cacheKey != theirs.cacheKey)
	}

	/// A ref for a server that is no longer configured has no URL to build, and
	/// must not crash trying.
	@Test func anUnknownServerYieldsNothing() {
		#expect(urls().source(ItemRef(server: ServerId(), id: "42"), size: 144) == nil)
		#expect(urls().source(nil, size: 144) == nil)
	}
}
